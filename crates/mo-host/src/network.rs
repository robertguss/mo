use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
use std::io::{self, BufReader, ErrorKind, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
#[cfg(unix)]
use std::os::fd::AsFd;
use std::path::Path;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use mo_bytecode::Capability;
use mo_bytecode::types::TypeTable;
use mo_def::HostOpId;
use mo_def::limits::{
    MO_LIMIT_FAKE_QUOTE, MO_LIMIT_HOST_NAME, MO_LIMIT_LOOKUP_ADDRESSES, MO_LIMIT_LOOKUPS,
    MO_LIMIT_OPEN_CONNECTIONS, MO_LIMIT_RECEIVE_BYTES, MO_LIMIT_RECORDED_CALLS,
};
use mo_heap::{Heap, ScratchGuard, Value};
use rustls::pki_types::{CertificateDer, ServerName};
use rustls::{AlertDescription, CertificateError, ClientConfig, ClientConnection, RootCertStore};

use crate::HostError;
use crate::files_common::find_case_tag;

#[derive(Clone, Debug, PartialEq, Eq)]
struct TargetRule {
    host: String,
    port: u16, // 0 = any port 1..=65535
}

struct RecordedCallEntry {
    method: String,
    url: String,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

#[derive(Clone)]
struct RecordedCall {
    method: Value,
    url: Value,
    headers: Vec<(Value, Value)>,
    body: Value,
}

enum ScriptedReply {
    Respond(Value),
    Fail(Value),
    TruncateAfter(i64),
}

struct ScriptEntry {
    method: String,
    url_prefix: String,
    replies: Vec<ScriptedReply>,
    cursor: usize,
}

struct FakeScriptedState {
    scripts: Vec<ScriptEntry>,
    recorded: Vec<RecordedCall>,
}

enum NetworkCap {
    Real {
        allowed: Option<Vec<TargetRule>>,
    },
    Fake {
        peers: Value, // Map(Text, Map(Bytes, Bytes))
        recorded: Rc<RefCell<Vec<RecordedCall>>>,
        allowed: Option<Vec<TargetRule>>,
    },
    FakeScripted {
        state: Rc<RefCell<FakeScriptedState>>,
        allowed: Option<Vec<TargetRule>>,
    },
}

struct RealTls {
    stream: TcpStream,
    conn: ClientConnection,
}

impl Drop for RealTls {
    fn drop(&mut self) {
        self.conn.send_close_notify();
        let _ = self.conn.write_tls(&mut self.stream);
    }
}

enum RealTransport {
    Plain(TcpStream),
    Tls(Box<RealTls>),
}

struct RealConnection {
    transport: RealTransport,
    deadline: Instant,
    open_connections: Rc<Cell<u32>>,
}

impl Drop for RealConnection {
    fn drop(&mut self) {
        let cur = self.open_connections.get();
        debug_assert!(cur != 0);
        self.open_connections.set(cur.saturating_sub(1));
    }
}

struct FakeConnection {
    replies: Value, // Map(Bytes, Bytes)
    recorded: Rc<RefCell<Vec<RecordedCall>>>,
    secure: bool,
    host: String,
    port: u16,
    recorded_yet: bool,
    sent: Vec<u8>,
    position: i64, // -1 before first receive
}

enum ActiveScriptedReply {
    Pending,
    Refused,
    Failed(Value),
    Closed,
    Bytes {
        wire: Value,
        pos: usize,
        fill_left: u64,
    },
}

struct ScriptedConnection {
    state: Rc<RefCell<FakeScriptedState>>,
    secure: bool,
    host_val: Value,
    host: String,
    port: u16,
    sent: Vec<u8>,
    active: ActiveScriptedReply,
}

enum ConnectionCap {
    Real(RefCell<RealConnection>),
    Fake(RefCell<FakeConnection>),
    Scripted(RefCell<ScriptedConnection>),
}

#[derive(Debug)]
enum NetErr {
    TimedOut,
    Closed,
    Refused,
    UnknownHost,
    Broken(String),
    Certificate(String),
}

/// Errors from [`OutboundStream`] host TCP/TLS streaming outside the Mo VM.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OutboundError {
    TimedOut,
    Closed,
    Refused,
    UnknownHost,
    Certificate(String),
    Broken(String),
}

impl From<NetErr> for OutboundError {
    fn from(err: NetErr) -> Self {
        match err {
            NetErr::TimedOut => Self::TimedOut,
            NetErr::Closed => Self::Closed,
            NetErr::Refused => Self::Refused,
            NetErr::UnknownHost => Self::UnknownHost,
            NetErr::Certificate(msg) => Self::Certificate(msg),
            NetErr::Broken(msg) => Self::Broken(msg),
        }
    }
}

/// Pure-safe-Rust streaming TCP/TLS client with per-operation idle timeout reset and overall ceiling.
pub struct OutboundStream {
    transport: RealTransport,
    idle_timeout: Duration,
    total_deadline: Instant,
}

fn saturating_deadline(timeout: Duration) -> Instant {
    let now = Instant::now();
    now.checked_add(timeout)
        .unwrap_or_else(|| now + Duration::from_secs(365 * 24 * 3600 * 100))
}

impl OutboundStream {
    /// Connects to `host:port` (with TLS via `rustls` when `secure` is true) within
    /// `connect_timeout` and `total_timeout`, then applies `idle_timeout` on each send/recv call.
    pub fn connect(
        host: &str,
        port: u16,
        secure: bool,
        connect_timeout: Duration,
        idle_timeout: Duration,
        total_timeout: Duration,
    ) -> Result<Self, OutboundError> {
        if port == 0 || !host_named(host.as_bytes()) {
            return Err(OutboundError::UnknownHost);
        }
        if connect_timeout.is_zero() || total_timeout.is_zero() {
            return Err(OutboundError::TimedOut);
        }
        let total_deadline = saturating_deadline(total_timeout);
        let connect_deadline = saturating_deadline(connect_timeout).min(total_deadline);
        let addrs = resolve_host_with_deadline(host, port, connect_deadline)?;
        let stream = open_tcp_socket(&addrs, connect_deadline)?;
        let transport = if secure {
            RealTransport::Tls(Box::new(tls_handshake(stream, host, connect_deadline)?))
        } else {
            RealTransport::Plain(stream)
        };
        Ok(Self {
            transport,
            idle_timeout,
            total_deadline,
        })
    }

    fn op_deadline(&self) -> Instant {
        saturating_deadline(self.idle_timeout).min(self.total_deadline)
    }

    /// Writes all of `bytes`, bounded by `idle_timeout` and `total_deadline`, aborting early if `stopping` turns true.
    pub fn send_all(&mut self, bytes: &[u8], stopping: &AtomicBool) -> Result<(), OutboundError> {
        let deadline = self.op_deadline();
        transport_send_all(&mut self.transport, bytes, deadline, Some(stopping))
            .map_err(OutboundError::from)
    }

    /// Reads up to `buf.len()` bytes with a fresh per-chunk `idle_timeout` bounded by `total_deadline`.
    pub fn recv_some(
        &mut self,
        buf: &mut [u8],
        stopping: &AtomicBool,
    ) -> Result<usize, OutboundError> {
        let deadline = self.op_deadline();
        transport_receive_some(&mut self.transport, buf, deadline, Some(stopping))
            .map_err(OutboundError::from)
    }
}

// ---------------------------------------------------------------------------
// Bounded DNS lookup worker pool (MO_LIMIT_LOOKUPS = 8)
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum LookupState {
    Free,
    Running,
    Done,
    Abandoned,
}

enum LookupOutcome {
    Addresses(Vec<SocketAddr>),
    UnknownHost,
    Broken(String),
}

struct LookupSlot {
    state: LookupState,
    outcome: Option<LookupOutcome>,
}

struct LookupPool {
    slots: Vec<Arc<(Mutex<LookupSlot>, Condvar)>>,
}

static LOOKUP_POOL: std::sync::LazyLock<Mutex<LookupPool>> = std::sync::LazyLock::new(|| {
    let mut slots = Vec::with_capacity(MO_LIMIT_LOOKUPS as usize);
    for _ in 0..MO_LIMIT_LOOKUPS {
        slots.push(Arc::new((
            Mutex::new(LookupSlot {
                state: LookupState::Free,
                outcome: None,
            }),
            Condvar::new(),
        )));
    }
    Mutex::new(LookupPool { slots })
});

fn classify_dns_error(err: &io::Error) -> LookupOutcome {
    if err.raw_os_error().is_some() {
        return LookupOutcome::Broken(err.to_string());
    }
    let msg = err.to_string();
    let lower = msg.to_ascii_lowercase();
    if lower.contains("name or service not known")
        || lower.contains("no address associated")
        || lower.contains("nodename nor servname")
        || lower.contains("no such host")
        || lower.contains("unknown host")
    {
        LookupOutcome::UnknownHost
    } else {
        LookupOutcome::Broken(msg)
    }
}

fn resolve_host_with_deadline(
    host: &str,
    port: u16,
    deadline: Instant,
) -> Result<Vec<SocketAddr>, NetErr> {
    let claimed = {
        let pool = LOOKUP_POOL
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let mut chosen = None;
        for slot_arc in &pool.slots {
            let mut slot = slot_arc
                .0
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if slot.state == LookupState::Free {
                slot.state = LookupState::Running;
                slot.outcome = None;
                chosen = Some(Arc::clone(slot_arc));
                break;
            }
        }
        chosen
    };

    let Some(slot_arc) = claimed else {
        return Err(NetErr::Broken("too many lookups still running".to_string()));
    };

    let host_owned = host.to_string();
    let worker_slot = Arc::clone(&slot_arc);
    let spawn_res = std::thread::Builder::new()
        .name("mo-dns".to_string())
        .spawn(move || {
            let outcome = match (host_owned.as_str(), port).to_socket_addrs() {
                Ok(iter) => {
                    let addrs: Vec<SocketAddr> =
                        iter.take(MO_LIMIT_LOOKUP_ADDRESSES as usize).collect();
                    if addrs.is_empty() {
                        LookupOutcome::UnknownHost
                    } else {
                        LookupOutcome::Addresses(addrs)
                    }
                }
                Err(e) => classify_dns_error(&e),
            };
            let (lock, cvar) = &*worker_slot;
            let mut slot = lock
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if slot.state == LookupState::Abandoned {
                slot.state = LookupState::Free;
                slot.outcome = None;
            } else {
                slot.outcome = Some(outcome);
                slot.state = LookupState::Done;
                cvar.notify_one();
            }
        });

    if let Err(e) = spawn_res {
        let mut slot = slot_arc
            .0
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        slot.state = LookupState::Free;
        slot.outcome = None;
        return Err(NetErr::Broken(e.to_string()));
    }

    let (lock, cvar) = &*slot_arc;
    let mut slot = lock
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    while slot.state == LookupState::Running {
        let now = Instant::now();
        if now >= deadline {
            slot.state = LookupState::Abandoned;
            return Err(NetErr::TimedOut);
        }
        let rem = deadline - now;
        let (guard, wait_res) = cvar
            .wait_timeout(slot, rem)
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        slot = guard;
        if slot.state == LookupState::Running && wait_res.timed_out() {
            slot.state = LookupState::Abandoned;
            return Err(NetErr::TimedOut);
        }
    }
    let Some(outcome) = slot.outcome.take() else {
        slot.state = LookupState::Free;
        return Err(NetErr::Broken(
            "lookup completed without outcome".to_string(),
        ));
    };
    slot.state = LookupState::Free;
    drop(slot);

    match outcome {
        LookupOutcome::Addresses(addrs) => Ok(addrs),
        LookupOutcome::UnknownHost => Err(NetErr::UnknownHost),
        LookupOutcome::Broken(msg) => Err(NetErr::Broken(msg)),
    }
}

// ---------------------------------------------------------------------------
// Hostname & allowlist rules (exact port of src/host/network.c)
// ---------------------------------------------------------------------------

fn host_named(bytes: &[u8]) -> bool {
    !bytes.is_empty() && bytes.len() <= MO_LIMIT_HOST_NAME as usize && !bytes.contains(&0)
}

fn parse_port_digits(bytes: &[u8]) -> Option<u16> {
    if bytes.is_empty() || bytes.len() > 5 {
        return None;
    }
    let mut port: u32 = 0;
    for &b in bytes {
        if !b.is_ascii_digit() {
            return None;
        }
        port = port * 10 + u32::from(b - b'0');
    }
    if (1..=65535).contains(&port) {
        Some(port as u16)
    } else {
        None
    }
}

fn parse_target(s: &str) -> Option<TargetRule> {
    let bytes = s.as_bytes();
    if bytes.is_empty() || bytes.contains(&0) {
        return None;
    }
    let (host_bytes, port) = if bytes[0] == b'[' {
        let close = bytes[1..].iter().position(|&b| b == b']')? + 1;
        if close == 1 {
            return None;
        }
        let inner = &bytes[1..close];
        if inner.contains(&b'[') || !inner.contains(&b':') {
            return None;
        }
        let consumed = close + 1;
        let port = if consumed < bytes.len() {
            if bytes[consumed] != b':' {
                return None;
            }
            parse_port_digits(&bytes[consumed + 1..])?
        } else {
            0
        };
        (inner, port)
    } else {
        if bytes.contains(&b'[') || bytes.contains(&b']') {
            return None;
        }
        let mut colons = 0usize;
        let mut last_colon = 0usize;
        for (i, &b) in bytes.iter().enumerate() {
            if b == b':' {
                colons += 1;
                last_colon = i;
            }
        }
        if colons == 1 {
            let port = parse_port_digits(&bytes[last_colon + 1..])?;
            (&bytes[..last_colon], port)
        } else {
            (bytes, 0)
        }
    };
    if !host_named(host_bytes) {
        return None;
    }
    let host_str = std::str::from_utf8(host_bytes).ok()?.to_ascii_lowercase();
    Some(TargetRule {
        host: host_str,
        port,
    })
}

fn push_unique_rule(rules: &mut Vec<TargetRule>, rule: TargetRule) {
    if !rules.contains(&rule) {
        rules.push(rule);
    }
}

fn is_host_allowed(allowed: &Option<Vec<TargetRule>>, host: &str, port: u16) -> bool {
    let Some(rules) = allowed else {
        return true;
    };
    if !host_named(host.as_bytes()) {
        return false;
    }
    let lower = host.to_ascii_lowercase();
    rules
        .iter()
        .any(|r| r.host == lower && (r.port == 0 || r.port == port))
}

fn is_loopback_host(host: &str) -> bool {
    matches!(host, "127.0.0.1" | "localhost" | "::1")
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum LoopbackAccess {
    Denied,
    AllPorts,
    Ports(Vec<u16>),
}

pub(crate) fn network_loopback_access(cap_val: &Value, require_real: bool) -> LoopbackAccess {
    let net = crate::downcast_cap::<NetworkCap>(cap_val, "Network capability");
    let allowed = match net {
        NetworkCap::Real { allowed } => allowed,
        NetworkCap::Fake { allowed, .. } | NetworkCap::FakeScripted { allowed, .. } => {
            if require_real {
                return LoopbackAccess::Denied;
            }
            allowed
        }
    };
    let Some(rules) = allowed else {
        return LoopbackAccess::AllPorts;
    };
    let mut ports = Vec::new();
    let mut any_loopback = false;
    for r in rules.iter().filter(|r| is_loopback_host(&r.host)) {
        any_loopback = true;
        if r.port == 0 {
            return LoopbackAccess::AllPorts;
        }
        ports.push(r.port);
    }
    if !any_loopback {
        return LoopbackAccess::Denied;
    }
    ports.sort_unstable();
    ports.dedup();
    LoopbackAccess::Ports(ports)
}

// ---------------------------------------------------------------------------
// Real non-blocking socket + pure-Rust rustls TLS helpers
// ---------------------------------------------------------------------------

#[cfg(unix)]
fn wait_fd(
    fd: std::os::fd::BorrowedFd<'_>,
    events: rustix::event::PollFlags,
    deadline: Instant,
) -> Result<(), NetErr> {
    wait_fd_interruptible(fd, events, deadline, None)
}

#[cfg(unix)]
fn wait_fd_interruptible(
    fd: std::os::fd::BorrowedFd<'_>,
    events: rustix::event::PollFlags,
    deadline: Instant,
    stopping: Option<&AtomicBool>,
) -> Result<(), NetErr> {
    use rustix::event::{PollFd, Timespec, poll};
    use rustix::io::Errno;

    loop {
        if stopping.is_some_and(|flag| flag.load(Ordering::Relaxed)) {
            return Err(NetErr::Closed);
        }
        let now = Instant::now();
        if now >= deadline {
            return Err(NetErr::TimedOut);
        }
        let rem = deadline - now;
        let slice = if stopping.is_some() {
            rem.min(Duration::from_millis(200))
        } else {
            rem
        };
        let ts = Timespec {
            tv_sec: i64::try_from(slice.as_secs()).unwrap_or(i64::MAX),
            tv_nsec: i64::from(slice.subsec_nanos()),
        };
        let mut fds = [PollFd::new(&fd, events)];
        match poll(&mut fds, Some(&ts)) {
            Ok(n) if n > 0 => return Ok(()),
            Ok(_) => continue,
            Err(Errno::INTR) => continue,
            Err(e) => return Err(NetErr::Broken(e.to_string())),
        }
    }
}

fn open_tcp_socket(addrs: &[SocketAddr], deadline: Instant) -> Result<TcpStream, NetErr> {
    let mut refused = false;
    let mut last_err: Option<io::Error> = None;

    for addr in addrs {
        let now = Instant::now();
        if now >= deadline {
            return Err(NetErr::TimedOut);
        }
        let rem = deadline - now;
        match TcpStream::connect_timeout(addr, rem) {
            Ok(stream) => {
                stream
                    .set_nonblocking(true)
                    .map_err(|e| NetErr::Broken(e.to_string()))?;
                return Ok(stream);
            }
            Err(e) if e.kind() == ErrorKind::TimedOut => return Err(NetErr::TimedOut),
            Err(e) if e.kind() == ErrorKind::ConnectionRefused => {
                refused = true;
                last_err = Some(e);
            }
            Err(e) => {
                last_err = Some(e);
            }
        }
    }

    if refused {
        Err(NetErr::Refused)
    } else {
        let msg = last_err
            .map(|e| e.to_string())
            .unwrap_or_else(|| "connection failed".to_string());
        Err(NetErr::Broken(msg))
    }
}

fn load_pem_certs_from_file(path: &Path, store: &mut RootCertStore) -> Result<usize, String> {
    let file = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let mut reader = BufReader::new(file);
    let mut loaded = 0usize;
    for cert_res in rustls_pemfile::certs(&mut reader) {
        let cert: CertificateDer<'static> = cert_res.map_err(|e| e.to_string())?;
        if store.add(cert).is_ok() {
            loaded += 1;
        }
    }
    Ok(loaded)
}

#[derive(PartialEq, Eq)]
struct EnvRootStoreKey {
    cert_file: Option<std::ffi::OsString>,
    cert_dir: Option<std::ffi::OsString>,
    file_stamp: Option<(u64, Option<std::time::SystemTime>)>,
    dir_stamp: Option<Option<std::time::SystemTime>>,
}

struct EnvRootStoreCache {
    key: EnvRootStoreKey,
    store: Arc<RootCertStore>,
}

static ENV_ROOT_STORE_CACHE: std::sync::LazyLock<std::sync::Mutex<Option<EnvRootStoreCache>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(None));

static DEFAULT_ROOT_STORE: std::sync::LazyLock<Result<Arc<RootCertStore>, String>> =
    std::sync::LazyLock::new(|| {
        let mut store = RootCertStore::empty();
        let native = rustls_native_certs::load_native_certs();
        let (added, _) = store.add_parsable_certificates(native.certs);
        if added == 0 {
            store.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
        }
        if store.is_empty() {
            return Err("could not load root certificates".to_string());
        }
        Ok(Arc::new(store))
    });

fn load_root_store() -> Result<Arc<RootCertStore>, NetErr> {
    let cert_file = std::env::var_os("SSL_CERT_FILE").filter(|v| !v.is_empty());
    let cert_dir = std::env::var_os("SSL_CERT_DIR").filter(|v| !v.is_empty());

    if cert_file.is_some() || cert_dir.is_some() {
        let file_stamp = cert_file
            .as_ref()
            .and_then(|f| std::fs::metadata(Path::new(f)).ok())
            .map(|m| (m.len(), m.modified().ok()));
        let dir_stamp = cert_dir
            .as_ref()
            .and_then(|d| std::fs::metadata(Path::new(d)).ok())
            .map(|m| m.modified().ok());
        let key = EnvRootStoreKey {
            cert_file: cert_file.clone(),
            cert_dir: cert_dir.clone(),
            file_stamp,
            dir_stamp,
        };
        if let Some(cached) = ENV_ROOT_STORE_CACHE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .as_ref()
            && cached.key == key
        {
            return Ok(Arc::clone(&cached.store));
        }

        let mut store = RootCertStore::empty();
        let mut loaded = 0usize;
        if let Some(ref f) = cert_file {
            match load_pem_certs_from_file(Path::new(f), &mut store) {
                Ok(n) => loaded += n,
                Err(_) if cert_dir.is_none() => {
                    return Err(NetErr::Certificate(
                        "could not load root certificates".to_string(),
                    ));
                }
                Err(_) => {}
            }
        }
        if let Some(ref d) = cert_dir
            && let Ok(entries) = std::fs::read_dir(Path::new(d))
        {
            for entry in entries.flatten() {
                let p = entry.path();
                if p.is_file()
                    && let Ok(n) = load_pem_certs_from_file(&p, &mut store)
                {
                    loaded += n;
                }
            }
        }
        if loaded == 0 {
            return Err(NetErr::Certificate(
                "could not load root certificates".to_string(),
            ));
        }
        let arc = Arc::new(store);
        *ENV_ROOT_STORE_CACHE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(EnvRootStoreCache {
            key,
            store: Arc::clone(&arc),
        });
        return Ok(arc);
    }

    match &*DEFAULT_ROOT_STORE {
        Ok(store) => Ok(Arc::clone(store)),
        Err(msg) => Err(NetErr::Certificate(msg.clone())),
    }
}

fn map_rustls_error(err: rustls::Error) -> NetErr {
    map_rustls_error_for_host(err, false)
}

fn map_rustls_error_for_host(err: rustls::Error, is_ip: bool) -> NetErr {
    match err {
        rustls::Error::InvalidCertificate(cert_err) => {
            let reason = match cert_err {
                CertificateError::Expired | CertificateError::ExpiredContext { .. } => {
                    "certificate has expired".to_string()
                }
                CertificateError::NotValidYet | CertificateError::NotValidYetContext { .. } => {
                    "certificate is not yet valid".to_string()
                }
                CertificateError::NotValidForName
                | CertificateError::NotValidForNameContext { .. } => {
                    if is_ip {
                        "IP address mismatch".to_string()
                    } else {
                        "hostname mismatch".to_string()
                    }
                }
                CertificateError::UnknownIssuer => {
                    "self-signed certificate or unknown issuer".to_string()
                }
                other => {
                    let dbg = format!("{other:?}");
                    if dbg.contains("CaUsedAsEndEntity") {
                        "self-signed certificate (CA used as end entity)".to_string()
                    } else {
                        format!("certificate verification failed: {dbg}")
                    }
                }
            };
            NetErr::Certificate(reason)
        }
        rustls::Error::AlertReceived(
            AlertDescription::BadCertificate
            | AlertDescription::UnsupportedCertificate
            | AlertDescription::CertificateRevoked
            | AlertDescription::CertificateExpired
            | AlertDescription::CertificateUnknown
            | AlertDescription::UnknownCA
            | AlertDescription::BadCertificateStatusResponse
            | AlertDescription::CertificateRequired,
        ) => NetErr::Certificate(err.to_string()),
        rustls::Error::AlertReceived(AlertDescription::CloseNotify) => NetErr::Closed,
        other => NetErr::Broken(other.to_string()),
    }
}

fn tls_flush_writes_interruptible(
    tls: &mut RealTls,
    deadline: Instant,
    stopping: Option<&AtomicBool>,
) -> Result<(), NetErr> {
    while tls.conn.wants_write() {
        if stopping.is_some_and(|flag| flag.load(Ordering::Relaxed)) {
            return Err(NetErr::Closed);
        }
        match tls.conn.write_tls(&mut tls.stream) {
            Ok(0) => return Err(NetErr::Closed),
            Ok(_) => {}
            Err(e) if e.kind() == ErrorKind::WouldBlock => {
                #[cfg(unix)]
                wait_fd_interruptible(
                    tls.stream.as_fd(),
                    rustix::event::PollFlags::OUT,
                    deadline,
                    stopping,
                )?;
                #[cfg(not(unix))]
                if Instant::now() >= deadline {
                    return Err(NetErr::TimedOut);
                }
            }
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(e) if matches!(e.kind(), ErrorKind::BrokenPipe | ErrorKind::ConnectionReset) => {
                return Err(NetErr::Closed);
            }
            Err(e) => return Err(NetErr::Broken(e.to_string())),
        }
    }
    Ok(())
}

fn tls_flush_writes(tls: &mut RealTls, deadline: Instant) -> Result<(), NetErr> {
    tls_flush_writes_interruptible(tls, deadline, None)
}

fn tls_handshake(stream: TcpStream, host: &str, deadline: Instant) -> Result<RealTls, NetErr> {
    let root_store = load_root_store()?;
    tls_handshake_with_store(stream, host, deadline, root_store)
}

fn tls_handshake_with_store(
    stream: TcpStream,
    host: &str,
    deadline: Instant,
    root_store: Arc<RootCertStore>,
) -> Result<RealTls, NetErr> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let config = ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13, &rustls::version::TLS12])
        .map_err(|e| NetErr::Broken(e.to_string()))?
        .with_root_certificates(root_store)
        .with_no_client_auth();

    let server_name = ServerName::try_from(host.to_string())
        .map_err(|_| NetErr::Broken("TLS host setup failed".to_string()))?;
    let is_ip = matches!(server_name, ServerName::IpAddress(_));

    let conn = ClientConnection::new(Arc::new(config), server_name)
        .map_err(|e| NetErr::Broken(e.to_string()))?;

    let mut tls = RealTls { stream, conn };

    while tls.conn.is_handshaking() {
        if tls.conn.wants_write() {
            match tls.conn.write_tls(&mut tls.stream) {
                Ok(0) => return Err(NetErr::Closed),
                Ok(_) => continue,
                Err(e) if e.kind() == ErrorKind::WouldBlock => {
                    #[cfg(unix)]
                    {
                        let mut flags = rustix::event::PollFlags::OUT;
                        if tls.conn.wants_read() {
                            flags |= rustix::event::PollFlags::IN;
                        }
                        wait_fd(tls.stream.as_fd(), flags, deadline)?;
                    }
                    continue;
                }
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(e)
                    if matches!(e.kind(), ErrorKind::BrokenPipe | ErrorKind::ConnectionReset) =>
                {
                    return Err(NetErr::Closed);
                }
                Err(e) => return Err(NetErr::Broken(e.to_string())),
            }
        }
        if tls.conn.wants_read() {
            match tls.conn.read_tls(&mut tls.stream) {
                Ok(0) => return Err(NetErr::Closed),
                Ok(_) => {
                    tls.conn
                        .process_new_packets()
                        .map_err(|e| map_rustls_error_for_host(e, is_ip))?;
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock => {
                    #[cfg(unix)]
                    wait_fd(tls.stream.as_fd(), rustix::event::PollFlags::IN, deadline)?;
                    continue;
                }
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(e)
                    if matches!(
                        e.kind(),
                        ErrorKind::ConnectionReset
                            | ErrorKind::BrokenPipe
                            | ErrorKind::UnexpectedEof
                    ) =>
                {
                    return Err(NetErr::Closed);
                }
                Err(e) => return Err(NetErr::Broken(e.to_string())),
            }
        }
    }

    tls_flush_writes(&mut tls, deadline)?;
    Ok(tls)
}

fn real_send_all(conn: &mut RealConnection, bytes: &[u8]) -> Result<(), NetErr> {
    transport_send_all(&mut conn.transport, bytes, conn.deadline, None)
}

fn transport_send_all(
    transport: &mut RealTransport,
    bytes: &[u8],
    deadline: Instant,
    stopping: Option<&AtomicBool>,
) -> Result<(), NetErr> {
    if stopping.is_some_and(|flag| flag.load(Ordering::Relaxed)) {
        return Err(NetErr::Closed);
    }
    if Instant::now() >= deadline {
        return Err(NetErr::TimedOut);
    }
    if bytes.is_empty() {
        return Ok(());
    }
    match transport {
        RealTransport::Plain(stream) => {
            let mut sent = 0usize;
            while sent < bytes.len() {
                #[cfg(unix)]
                wait_fd_interruptible(
                    stream.as_fd(),
                    rustix::event::PollFlags::OUT,
                    deadline,
                    stopping,
                )?;
                #[cfg(unix)]
                {
                    use rustix::io::Errno;
                    use rustix::net::{SendFlags, send};
                    #[cfg(target_os = "linux")]
                    let flags = SendFlags::NOSIGNAL;
                    #[cfg(not(target_os = "linux"))]
                    let flags = SendFlags::empty();
                    match send(stream.as_fd(), &bytes[sent..], flags) {
                        Ok(n) => sent += n,
                        Err(Errno::PIPE | Errno::CONNRESET) => return Err(NetErr::Closed),
                        #[expect(
                            unreachable_patterns,
                            reason = "EAGAIN and EWOULDBLOCK are identical on Linux but distinct on some Unix targets"
                        )]
                        Err(Errno::INTR | Errno::AGAIN | Errno::WOULDBLOCK) => continue,
                        Err(e) => return Err(NetErr::Broken(e.to_string())),
                    }
                }
                #[cfg(not(unix))]
                match stream.write(&bytes[sent..]) {
                    Ok(n) => sent += n,
                    Err(e)
                        if matches!(
                            e.kind(),
                            ErrorKind::BrokenPipe | ErrorKind::ConnectionReset
                        ) =>
                    {
                        return Err(NetErr::Closed);
                    }
                    Err(e)
                        if matches!(e.kind(), ErrorKind::Interrupted | ErrorKind::WouldBlock) =>
                    {
                        continue;
                    }
                    Err(e) => return Err(NetErr::Broken(e.to_string())),
                }
            }
            Ok(())
        }
        RealTransport::Tls(tls) => {
            let mut written = 0usize;
            while written < bytes.len() {
                let n = tls
                    .conn
                    .writer()
                    .write(&bytes[written..])
                    .map_err(|e| NetErr::Broken(e.to_string()))?;
                written += n;
                tls_flush_writes_interruptible(tls, deadline, stopping)?;
            }
            Ok(())
        }
    }
}

fn real_receive_some(conn: &mut RealConnection, buf: &mut [u8]) -> Result<usize, NetErr> {
    transport_receive_some(&mut conn.transport, buf, conn.deadline, None)
}

fn transport_receive_some(
    transport: &mut RealTransport,
    buf: &mut [u8],
    deadline: Instant,
    stopping: Option<&AtomicBool>,
) -> Result<usize, NetErr> {
    if stopping.is_some_and(|flag| flag.load(Ordering::Relaxed)) {
        return Err(NetErr::Closed);
    }
    if Instant::now() >= deadline {
        return Err(NetErr::TimedOut);
    }
    match transport {
        RealTransport::Plain(stream) => loop {
            #[cfg(unix)]
            wait_fd_interruptible(
                stream.as_fd(),
                rustix::event::PollFlags::IN,
                deadline,
                stopping,
            )?;
            match stream.read(buf) {
                Ok(n) => return Ok(n),
                Err(e) if e.kind() == ErrorKind::ConnectionReset => return Err(NetErr::Closed),
                Err(e) if matches!(e.kind(), ErrorKind::Interrupted | ErrorKind::WouldBlock) => {
                    continue;
                }
                Err(e) => return Err(NetErr::Broken(e.to_string())),
            }
        },
        RealTransport::Tls(tls) => loop {
            if stopping.is_some_and(|flag| flag.load(Ordering::Relaxed)) {
                return Err(NetErr::Closed);
            }
            match tls.conn.reader().read(buf) {
                Ok(n) => return Ok(n),
                Err(e) if e.kind() == ErrorKind::WouldBlock => {}
                Err(e)
                    if matches!(
                        e.kind(),
                        ErrorKind::UnexpectedEof | ErrorKind::ConnectionReset
                    ) =>
                {
                    return Err(NetErr::Closed);
                }
                Err(e) => return Err(NetErr::Broken(e.to_string())),
            }

            if tls.conn.wants_write() {
                tls_flush_writes_interruptible(tls, deadline, stopping)?;
            }

            match tls.conn.read_tls(&mut tls.stream) {
                Ok(0) => {
                    let _ = tls.conn.process_new_packets().map_err(map_rustls_error)?;
                    match tls.conn.reader().read(buf) {
                        Ok(n) => return Ok(n),
                        Err(e) if e.kind() == ErrorKind::WouldBlock => return Err(NetErr::Closed),
                        Err(e)
                            if matches!(
                                e.kind(),
                                ErrorKind::UnexpectedEof | ErrorKind::ConnectionReset
                            ) =>
                        {
                            return Err(NetErr::Closed);
                        }
                        Err(e) => return Err(NetErr::Broken(e.to_string())),
                    }
                }
                Ok(_) => {
                    tls.conn.process_new_packets().map_err(map_rustls_error)?;
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock => {
                    #[cfg(unix)]
                    wait_fd_interruptible(
                        tls.stream.as_fd(),
                        rustix::event::PollFlags::IN,
                        deadline,
                        stopping,
                    )?;
                }
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(e)
                    if matches!(e.kind(), ErrorKind::ConnectionReset | ErrorKind::BrokenPipe) =>
                {
                    return Err(NetErr::Closed);
                }
                Err(e) => return Err(NetErr::Broken(e.to_string())),
            }
        },
    }
}

// ---------------------------------------------------------------------------
// Fake network mismatch description (exact port of src/host/fake-network.c)
// ---------------------------------------------------------------------------

fn quote_line(out: &mut String, line: Option<&[u8]>) {
    let Some(bytes) = line else {
        out.push_str("nothing more");
        return;
    };
    out.push('"');
    let cap = MO_LIMIT_FAKE_QUOTE as usize;
    for &b in bytes.iter().take(cap) {
        match b {
            b'\r' => out.push_str("\\r"),
            b'\n' => out.push_str("\\n"),
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            0x20..=0x7e => out.push(b as char),
            _ => {
                const HEX: &[u8; 16] = b"0123456789abcdef";
                out.push_str("\\x");
                out.push(HEX[(b >> 4) as usize] as char);
                out.push(HEX[(b & 0xf) as usize] as char);
            }
        }
    }
    if bytes.len() > cap {
        out.push_str("...\"");
    } else {
        out.push('"');
    }
}

fn next_line<'a>(bytes: &'a [u8], at: &mut usize) -> Option<&'a [u8]> {
    if *at >= bytes.len() {
        return None;
    }
    let rest = &bytes[*at..];
    let len = match rest.iter().position(|&b| b == b'\n') {
        Some(pos) => pos + 1,
        None => rest.len(),
    };
    let line = &rest[..len];
    *at += len;
    Some(line)
}

fn describe_fake_mismatch(replies: &Value, sent: &[u8]) -> String {
    let mut out = String::new();
    let mut at = 0usize;
    let Some(mut first) = next_line(sent, &mut at) else {
        return "no reply for a connection that sent nothing".to_string();
    };
    while let Some(&last) = first.last() {
        if last == b'\n' || last == b'\r' {
            first = &first[..first.len() - 1];
        } else {
            break;
        }
    }
    out.push_str("no reply for ");
    quote_line(&mut out, Some(first));

    let mut most = 0u32;
    let mut was: Option<&[u8]> = None;
    let mut wanted: Option<&[u8]> = None;

    for (key, _) in replies.as_map().iter() {
        let held = key.as_bytes();
        let mut mine = 0usize;
        let mut theirs = 0usize;
        let mut shared = 0u32;
        let mut a = next_line(sent, &mut mine);
        let mut b = next_line(held, &mut theirs);
        while a.is_some() && a == b {
            shared += 1;
            a = next_line(sent, &mut mine);
            b = next_line(held, &mut theirs);
        }
        if shared > most {
            most = shared;
            was = a;
            wanted = b;
        }
    }
    if most == 0 {
        return out;
    }
    out.push_str(": sent ");
    quote_line(&mut out, was);
    out.push_str(", expected ");
    quote_line(&mut out, wanted);
    out
}

// ---------------------------------------------------------------------------
// Result value builders
// ---------------------------------------------------------------------------

fn make_network_ok(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    value: Value,
) -> Result<Value, HostError> {
    let ok_tag = find_case_tag(types, result_ty, "Ok");
    Ok(heap.make_case(types, result_ty, ok_tag, vec![value])?)
}

fn make_network_error(
    heap: &Heap,
    types: &TypeTable,
    result_ty: u32,
    err: NetErr,
    host_val: &Value,
    port: i64,
) -> Result<Value, HostError> {
    let err_tag = find_case_tag(types, result_ty, "Error");
    let err_ty = types.get(result_ty).cases[err_tag as usize].fields[0].ty;
    let err_val = match err {
        NetErr::TimedOut => {
            let tag = find_case_tag(types, err_ty, "TimedOut");
            heap.make_case(types, err_ty, tag, Vec::new())?
        }
        NetErr::Closed => {
            let tag = find_case_tag(types, err_ty, "Closed");
            heap.make_case(types, err_ty, tag, Vec::new())?
        }
        NetErr::Refused => {
            let tag = find_case_tag(types, err_ty, "Refused");
            heap.make_case(types, err_ty, tag, vec![host_val.clone(), Value::Int(port)])?
        }
        NetErr::UnknownHost => {
            let tag = find_case_tag(types, err_ty, "UnknownHost");
            heap.make_case(types, err_ty, tag, vec![host_val.clone()])?
        }
        NetErr::Broken(reason) => {
            let tag = find_case_tag(types, err_ty, "Broken");
            let text = heap.alloc_text(reason)?;
            heap.make_case(types, err_ty, tag, vec![text])?
        }
        NetErr::Certificate(reason) => {
            let tag = find_case_tag(types, err_ty, "Certificate");
            let text = heap.alloc_text(reason)?;
            heap.make_case(types, err_ty, tag, vec![text])?
        }
    };
    Ok(heap.make_case(types, result_ty, err_tag, vec![err_val])?)
}

fn parse_sent_http_request(
    sent: &[u8],
    secure: bool,
    default_host: &str,
    port: u16,
) -> (RecordedCallEntry, String) {
    let scheme = if secure { "https" } else { "http" };
    let host = if default_host.parse::<std::net::Ipv6Addr>().is_ok() {
        format!("[{default_host}]")
    } else {
        default_host.to_string()
    };
    let default_auth = if (secure && port == 443) || (!secure && port == 80) {
        host
    } else {
        format!("{host}:{port}")
    };
    let Some(cut) = sent.windows(4).position(|w| w == b"\r\n\r\n") else {
        let url = format!("{scheme}://{default_auth}/");
        return (
            RecordedCallEntry {
                method: String::new(),
                url,
                headers: Vec::new(),
                body: sent.to_vec(),
            },
            "/".to_string(),
        );
    };
    let head = String::from_utf8_lossy(&sent[..cut]);
    let body = sent[cut + 4..].to_vec();
    let mut lines = head.split("\r\n");
    let request_line = lines.next().unwrap_or("");
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or("").to_string();
    let raw_target = parts.next().unwrap_or("/");
    let mut headers = Vec::new();
    for line in lines {
        if let Some((name, val)) = line.split_once(':') {
            let trimmed = val.trim().to_string();
            headers.push((name.to_string(), trimmed));
        }
    }
    // Always record the connected authority rather than a spoofable client `Host` header.
    let authority = default_auth;
    let path = if raw_target.starts_with('/') {
        raw_target.to_string()
    } else {
        format!("/{raw_target}")
    };
    let url = format!("{scheme}://{authority}{path}");
    (
        RecordedCallEntry {
            method,
            url,
            headers,
            body,
        },
        path,
    )
}

// ---------------------------------------------------------------------------
// Public entry points for crates/mo-host
// ---------------------------------------------------------------------------

pub(crate) fn make_real_network(heap: &Heap, ty: u32) -> Result<Value, HostError> {
    Ok(heap.alloc_capability(
        Capability::Network,
        ty,
        Rc::new(NetworkCap::Real { allowed: None }),
    )?)
}

pub(crate) fn network_fake(heap: &Heap, result_ty: u32, peers: Value) -> Result<Value, HostError> {
    Ok(heap.alloc_capability(
        Capability::Network,
        result_ty,
        Rc::new(NetworkCap::Fake {
            peers,
            recorded: Rc::new(RefCell::new(Vec::new())),
            allowed: None,
        }),
    )?)
}

pub(crate) fn network_fake_scripted(
    heap: &Heap,
    result_ty: u32,
    scripts_val: &Value,
) -> Result<Value, HostError> {
    let mut scripts = Vec::new();
    for script_tup in scripts_val.as_list().iter() {
        let method = heap.extract_field(script_tup, 0).as_text().to_string();
        let url_prefix = heap.extract_field(script_tup, 1).as_text().to_string();
        let replies_val = heap.extract_field(script_tup, 2);
        let mut replies = Vec::new();
        for reply_tup in replies_val.as_list().iter() {
            let kind = heap.extract_field(reply_tup, 0).as_int();
            let count = heap.extract_field(reply_tup, 1).as_int();
            let err_val = heap.extract_field(reply_tup, 3);
            let item = match kind {
                0 => ScriptedReply::Respond(heap.extract_field(reply_tup, 2)),
                1 => ScriptedReply::Fail(err_val),
                _ => ScriptedReply::TruncateAfter(count),
            };
            replies.push(item);
        }
        scripts.push(ScriptEntry {
            method,
            url_prefix,
            replies,
            cursor: 0,
        });
    }
    let state = Rc::new(RefCell::new(FakeScriptedState {
        scripts,
        recorded: Vec::new(),
    }));
    Ok(heap.alloc_capability(
        Capability::Network,
        result_ty,
        Rc::new(NetworkCap::FakeScripted {
            state,
            allowed: None,
        }),
    )?)
}

fn chunked_request_len(mut body: &[u8]) -> Option<usize> {
    let total = body.len();
    loop {
        let end = body.windows(2).position(|w| w == b"\r\n")?;
        let line = std::str::from_utf8(&body[..end]).ok()?;
        let size = line.split(';').next()?.trim_end();
        let size = usize::from_str_radix(size, 16).ok()?;
        body = &body[end + 2..];
        if size == 0 {
            let trailers_len = if body.starts_with(b"\r\n") {
                2
            } else {
                body.windows(4).position(|w| w == b"\r\n\r\n")? + 4
            };
            return Some(total - body.len() + trailers_len);
        }
        body = body.get(size..)?.strip_prefix(b"\r\n")?;
    }
}

fn scripted_request_len(sent: &[u8]) -> Option<usize> {
    let head_end = sent.windows(4).position(|w| w == b"\r\n\r\n")?;
    let body_start = head_end + 4;
    let head = String::from_utf8_lossy(&sent[..head_end]);
    let mut headers = head
        .split("\r\n")
        .skip(1)
        .filter_map(|line| line.split_once(':'));
    let body = &sent[body_start..];
    let body_len = if headers.clone().any(|(name, value)| {
        name.eq_ignore_ascii_case("transfer-encoding")
            && value
                .split(',')
                .any(|coding| coding.trim().eq_ignore_ascii_case("chunked"))
    }) {
        chunked_request_len(body)?
    } else {
        match headers.find(|(name, _)| name.eq_ignore_ascii_case("content-length")) {
            Some((_, value)) => value.trim().parse::<usize>().ok()?,
            None => 0,
        }
    };
    (body_len <= body.len()).then(|| body_start + body_len)
}

fn resolve_scripted_reply(
    heap: &Heap,
    scripted: &mut ScriptedConnection,
    request_len: usize,
) -> Result<(), HostError> {
    let (entry, path) = parse_sent_http_request(
        &scripted.sent[..request_len],
        scripted.secure,
        &scripted.host,
        scripted.port,
    );
    scripted.sent = Vec::new();
    let mut state = scripted.state.borrow_mut();
    let matched_idx = state.scripts.iter().position(|script| {
        (script.method == "*" || script.method.eq_ignore_ascii_case(&entry.method))
            && scripted_url_matches(&entry.url, &path, &script.url_prefix)
    });
    push_recorded_call(heap, &mut state.recorded, entry)?;
    let Some(sidx) = matched_idx else {
        scripted.active = ActiveScriptedReply::Refused;
        return Ok(());
    };
    if state.scripts[sidx].replies.is_empty() {
        scripted.active = ActiveScriptedReply::Refused;
        return Ok(());
    }
    let ridx = state.scripts[sidx]
        .cursor
        .min(state.scripts[sidx].replies.len() - 1);
    if state.scripts[sidx].cursor + 1 < state.scripts[sidx].replies.len() {
        state.scripts[sidx].cursor += 1;
    }
    match &state.scripts[sidx].replies[ridx] {
        ScriptedReply::Fail(err) => scripted.active = ActiveScriptedReply::Failed(err.clone()),
        ScriptedReply::Respond(wire) => {
            scripted.active = ActiveScriptedReply::Bytes {
                wire: wire.clone(),
                pos: 0,
                fill_left: 0,
            }
        }
        ScriptedReply::TruncateAfter(n) => {
            let fill_left = (*n).max(0) as u64;
            let declared_len = fill_left + 64;
            let wire = format!("HTTP/1.1 200 OK\r\nContent-Length: {declared_len}\r\n\r\n");
            scripted.active = ActiveScriptedReply::Bytes {
                wire: heap.alloc_bytes(wire.into_bytes())?,
                pos: 0,
                fill_left,
            };
        }
    }
    Ok(())
}

fn push_recorded_call(
    heap: &Heap,
    recorded: &mut Vec<RecordedCall>,
    entry: RecordedCallEntry,
) -> Result<(), HostError> {
    if recorded.len() < MO_LIMIT_RECORDED_CALLS as usize {
        let mut headers = Vec::with_capacity(entry.headers.len());
        for (key, value) in entry.headers {
            headers.push((heap.alloc_text(key)?, heap.alloc_text(value)?));
        }
        recorded.push(RecordedCall {
            method: heap.alloc_text(entry.method)?,
            url: heap.alloc_text(entry.url)?,
            headers,
            body: heap.alloc_bytes(entry.body)?,
        });
    }
    Ok(())
}

fn scripted_url_matches(entry_url: &str, entry_path: &str, url_prefix: &str) -> bool {
    if url_prefix.is_empty() {
        return false;
    }
    if url_prefix.starts_with('/') {
        if !entry_path.starts_with(url_prefix) {
            return false;
        }
        let rest = &entry_path[url_prefix.len()..];
        return url_prefix == "/"
            || url_prefix.ends_with('/')
            || rest.is_empty()
            || matches!(rest.as_bytes()[0], b'/' | b'?' | b'#');
    }
    if !entry_url.starts_with(url_prefix) {
        return false;
    }
    let rest = &entry_url[url_prefix.len()..];
    url_prefix.ends_with('/') || rest.is_empty() || matches!(rest.as_bytes()[0], b'/' | b'?' | b'#')
}

pub(crate) fn call_network(
    heap: &Heap,
    types: &TypeTable,
    open_connections: &Rc<Cell<u32>>,
    op: HostOpId,
    result_ty: u32,
    args: &[Value],
) -> Result<Value, HostError> {
    match op {
        HostOpId::NetworkRecordedCalls => {
            let net = crate::downcast_cap::<NetworkCap>(&args[0], "Network capability");
            let entries = match net {
                NetworkCap::Real { .. } => {
                    return Err(HostError::Bug {
                        reason: "network observation needs a fake",
                        detail: None,
                    });
                }
                NetworkCap::Fake { recorded, .. } => recorded.borrow().clone(),
                NetworkCap::FakeScripted { state, .. } => state.borrow().recorded.clone(),
            };
            let outer_tup_ty = types.get(result_ty).parameters[0];
            #[expect(
                clippy::expect_used,
                reason = "verified bytecode guarantees tuple layout for recorded_calls_raw"
            )]
            let outer_tup_layout = types
                .find_tuple_layout(outer_tup_ty)
                .expect("outer tuple layout for recorded_calls_raw");
            let headers_list_ty = types.get(outer_tup_ty).parameters[2];
            let pair_tup_ty = types.get(headers_list_ty).parameters[0];
            #[expect(
                clippy::expect_used,
                reason = "verified bytecode guarantees header pair tuple layout for recorded_calls_raw"
            )]
            let pair_tup_layout = types
                .find_tuple_layout(pair_tup_ty)
                .expect("header pair tuple layout for recorded_calls_raw");

            let mut out = VecDeque::with_capacity(entries.len());
            for entry in entries {
                let mut hdr_items = VecDeque::with_capacity(entry.headers.len());
                for (k, v) in entry.headers {
                    hdr_items.push_back(heap.construct(types, pair_tup_layout, vec![k, v])?);
                }
                let hdrs_val = heap.alloc_list(hdr_items)?;
                let tup = heap.construct(
                    types,
                    outer_tup_layout,
                    vec![entry.method, entry.url, hdrs_val, entry.body],
                )?;
                out.push_back(tup);
            }
            Ok(heap.alloc_list(out)?)
        }

        HostOpId::NetworkAllowHosts => {
            let net = crate::downcast_cap::<NetworkCap>(&args[0], "Network capability");
            let existing = match net {
                NetworkCap::Real { allowed }
                | NetworkCap::Fake { allowed, .. }
                | NetworkCap::FakeScripted { allowed, .. } => allowed,
            };
            let mut next_rules = Vec::new();
            for item in args[1].as_list().iter() {
                let Some(req) = parse_target(item.as_text()) else {
                    continue;
                };
                match existing {
                    None => push_unique_rule(&mut next_rules, req),
                    Some(prev) => {
                        for old in prev {
                            if old.host != req.host {
                                continue;
                            }
                            if old.port != 0 && req.port != 0 && old.port != req.port {
                                continue;
                            }
                            let narrowed = TargetRule {
                                host: req.host.clone(),
                                port: if old.port != 0 { old.port } else { req.port },
                            };
                            push_unique_rule(&mut next_rules, narrowed);
                        }
                    }
                }
            }
            let new_cap = match net {
                NetworkCap::Real { .. } => NetworkCap::Real {
                    allowed: Some(next_rules),
                },
                NetworkCap::Fake {
                    peers, recorded, ..
                } => NetworkCap::Fake {
                    peers: peers.clone(),
                    recorded: Rc::clone(recorded),
                    allowed: Some(next_rules),
                },
                NetworkCap::FakeScripted { state, .. } => NetworkCap::FakeScripted {
                    state: Rc::clone(state),
                    allowed: Some(next_rules),
                },
            };
            Ok(heap.alloc_capability(Capability::Network, result_ty, Rc::new(new_cap))?)
        }

        HostOpId::NetworkConnect | HostOpId::NetworkConnectSecure => {
            let secure = op == HostOpId::NetworkConnectSecure;
            let net = crate::downcast_cap::<NetworkCap>(&args[0], "Network capability");
            let host_str = args[1].as_text();
            let port = args[2].as_int();
            if !(1..=65535).contains(&port) {
                return Err(HostError::Bug {
                    reason: "port outside 1 to 65535",
                    detail: None,
                });
            }
            let port_u16 = port as u16;
            let allowed = match net {
                NetworkCap::Real { allowed }
                | NetworkCap::Fake { allowed, .. }
                | NetworkCap::FakeScripted { allowed, .. } => allowed,
            };
            if !is_host_allowed(allowed, host_str, port_u16) {
                return make_network_error(heap, types, result_ty, NetErr::Refused, &args[1], port);
            }

            let ok_tag = find_case_tag(types, result_ty, "Ok");
            let conn_ty = types.get(result_ty).cases[ok_tag as usize].fields[0].ty;

            match net {
                NetworkCap::Fake {
                    peers, recorded, ..
                } => {
                    if !host_named(host_str.as_bytes()) {
                        return make_network_error(
                            heap,
                            types,
                            result_ty,
                            NetErr::UnknownHost,
                            &args[1],
                            port,
                        );
                    }
                    let key_str = format!("{host_str}:{port_u16}");
                    let key_val = heap.alloc_text(key_str)?;
                    let Some(replies) = peers
                        .as_map()
                        .get(types, heap.hash_key(), &key_val)
                        .cloned()
                    else {
                        return make_network_error(
                            heap,
                            types,
                            result_ty,
                            NetErr::UnknownHost,
                            &args[1],
                            port,
                        );
                    };
                    let conn_cap = ConnectionCap::Fake(RefCell::new(FakeConnection {
                        replies,
                        recorded: Rc::clone(recorded),
                        secure,
                        host: host_str.to_string(),
                        port: port_u16,
                        recorded_yet: false,
                        sent: Vec::new(),
                        position: -1,
                    }));
                    let val =
                        heap.alloc_capability(Capability::Connection, conn_ty, Rc::new(conn_cap))?;
                    make_network_ok(heap, types, result_ty, val)
                }
                NetworkCap::FakeScripted { state, .. } => {
                    if !host_named(host_str.as_bytes()) {
                        return make_network_error(
                            heap,
                            types,
                            result_ty,
                            NetErr::UnknownHost,
                            &args[1],
                            port,
                        );
                    }
                    let conn_cap = ConnectionCap::Scripted(RefCell::new(ScriptedConnection {
                        state: Rc::clone(state),
                        secure,
                        host_val: args[1].clone(),
                        host: host_str.to_string(),
                        port: port_u16,
                        sent: Vec::new(),
                        active: ActiveScriptedReply::Pending,
                    }));
                    let val =
                        heap.alloc_capability(Capability::Connection, conn_ty, Rc::new(conn_cap))?;
                    make_network_ok(heap, types, result_ty, val)
                }
                NetworkCap::Real { .. } => {
                    if u64::from(open_connections.get()) >= MO_LIMIT_OPEN_CONNECTIONS {
                        return Err(HostError::Bug {
                            reason: "too many connections open at once",
                            detail: None,
                        });
                    }
                    if !host_named(host_str.as_bytes()) {
                        return make_network_error(
                            heap,
                            types,
                            result_ty,
                            NetErr::UnknownHost,
                            &args[1],
                            port,
                        );
                    }
                    let within_us = args[3].as_duration();
                    if within_us <= 0 {
                        return make_network_error(
                            heap,
                            types,
                            result_ty,
                            NetErr::TimedOut,
                            &args[1],
                            port,
                        );
                    }
                    let now = Instant::now();
                    let dur = Duration::from_micros(within_us as u64);
                    // Saturate far future durations cleanly without panicking.
                    let deadline = now
                        .checked_add(dur)
                        .unwrap_or_else(|| now + Duration::from_secs(365 * 24 * 3600 * 100));

                    let addrs = match resolve_host_with_deadline(host_str, port_u16, deadline) {
                        Ok(a) => a,
                        Err(err) => {
                            return make_network_error(heap, types, result_ty, err, &args[1], port);
                        }
                    };
                    let stream = match open_tcp_socket(&addrs, deadline) {
                        Ok(s) => s,
                        Err(err) => {
                            return make_network_error(heap, types, result_ty, err, &args[1], port);
                        }
                    };
                    let transport = if secure {
                        match tls_handshake(stream, host_str, deadline) {
                            Ok(tls) => RealTransport::Tls(Box::new(tls)),
                            Err(err) => {
                                return make_network_error(
                                    heap, types, result_ty, err, &args[1], port,
                                );
                            }
                        }
                    } else {
                        RealTransport::Plain(stream)
                    };

                    let real_conn = RealConnection {
                        transport,
                        deadline,
                        open_connections: Rc::clone(open_connections),
                    };
                    open_connections.set(open_connections.get() + 1);
                    let conn_cap = ConnectionCap::Real(RefCell::new(real_conn));
                    let val =
                        heap.alloc_capability(Capability::Connection, conn_ty, Rc::new(conn_cap))?;
                    make_network_ok(heap, types, result_ty, val)
                }
            }
        }

        HostOpId::ConnectionSend => {
            let conn_cap = crate::downcast_cap::<ConnectionCap>(&args[0], "Connection capability");
            let bytes = args[1].as_bytes();
            match conn_cap {
                ConnectionCap::Fake(cell) => {
                    let mut fake = cell.borrow_mut();
                    if fake.position >= 0 {
                        return make_network_error(
                            heap,
                            types,
                            result_ty,
                            NetErr::Closed,
                            &Value::Unit,
                            0,
                        );
                    }
                    if !bytes.is_empty() {
                        let new_len = fake.sent.len().saturating_add(bytes.len());
                        if new_len as u64 > mo_def::limits::MO_LIMIT_VALUE_BYTES {
                            return Err(HostError::OutOfMemory {
                                value_bytes_limit: true,
                            });
                        }
                        let mut scratch = ScratchGuard::new(heap.budget());
                        scratch.charge(new_len)?;
                        fake.sent.extend_from_slice(bytes);
                    }
                    make_network_ok(heap, types, result_ty, Value::Unit)
                }
                ConnectionCap::Scripted(cell) => {
                    let mut scripted = cell.borrow_mut();
                    if !matches!(scripted.active, ActiveScriptedReply::Pending) {
                        return make_network_error(
                            heap,
                            types,
                            result_ty,
                            NetErr::Closed,
                            &Value::Unit,
                            0,
                        );
                    }
                    if !bytes.is_empty() {
                        let new_len = scripted.sent.len().saturating_add(bytes.len());
                        if new_len as u64 > mo_def::limits::MO_LIMIT_VALUE_BYTES {
                            return Err(HostError::OutOfMemory {
                                value_bytes_limit: true,
                            });
                        }
                        let mut scratch = ScratchGuard::new(heap.budget());
                        scratch.charge(new_len)?;
                        scripted.sent.extend_from_slice(bytes);
                        drop(scratch);
                        if let Some(request_len) = scripted_request_len(&scripted.sent) {
                            resolve_scripted_reply(heap, &mut scripted, request_len)?;
                        }
                    }
                    make_network_ok(heap, types, result_ty, Value::Unit)
                }
                ConnectionCap::Real(cell) => {
                    let mut real = cell.borrow_mut();
                    match real_send_all(&mut real, bytes) {
                        Ok(()) => make_network_ok(heap, types, result_ty, Value::Unit),
                        Err(err) => {
                            make_network_error(heap, types, result_ty, err, &Value::Unit, 0)
                        }
                    }
                }
            }
        }

        HostOpId::ConnectionReceive => {
            let most = args[1].as_int();
            if most < 1 || (most as u64) > MO_LIMIT_RECEIVE_BYTES {
                return Err(HostError::Bug {
                    reason: "most below 1 or above the receive limit",
                    detail: None,
                });
            }
            let most_usize = most as usize;
            let conn_cap = crate::downcast_cap::<ConnectionCap>(&args[0], "Connection capability");
            match conn_cap {
                ConnectionCap::Fake(cell) => {
                    let mut fake = cell.borrow_mut();
                    if !fake.recorded_yet {
                        fake.recorded_yet = true;
                        let (entry, _) =
                            parse_sent_http_request(&fake.sent, fake.secure, &fake.host, fake.port);
                        push_recorded_call(heap, &mut fake.recorded.borrow_mut(), entry)?;
                    }
                    let pos = if fake.position < 0 { 0 } else { fake.position };
                    let sent_val = heap.alloc_bytes(fake.sent.clone())?;
                    let found = fake
                        .replies
                        .as_map()
                        .get(types, heap.hash_key(), &sent_val)
                        .cloned();
                    if let Some(reply_val) = found {
                        let reply_bytes = reply_val.as_bytes();
                        let pos_u = pos as usize;
                        let left = reply_bytes.len().saturating_sub(pos_u).min(most_usize);
                        let chunk = reply_bytes[pos_u..pos_u + left].to_vec();
                        let val = heap.alloc_bytes(chunk)?;
                        let res = make_network_ok(heap, types, result_ty, val)?;
                        fake.position = pos + left as i64;
                        Ok(res)
                    } else {
                        let reason = describe_fake_mismatch(&fake.replies, &fake.sent);
                        let res = make_network_error(
                            heap,
                            types,
                            result_ty,
                            NetErr::Broken(reason),
                            &Value::Unit,
                            0,
                        )?;
                        fake.position = pos;
                        Ok(res)
                    }
                }
                ConnectionCap::Scripted(cell) => {
                    let mut scripted = cell.borrow_mut();
                    let chunk = match &mut scripted.active {
                        ActiveScriptedReply::Bytes {
                            wire,
                            pos,
                            fill_left,
                        } => {
                            let wire = wire.as_bytes();
                            let take = (wire.len() - *pos).min(most_usize);
                            let fill = (*fill_left).min((most_usize - take) as u64) as usize;
                            let mut chunk = Vec::with_capacity(take + fill);
                            chunk.extend_from_slice(&wire[*pos..*pos + take]);
                            chunk.resize(take + fill, b'x');
                            *pos += take;
                            *fill_left -= fill as u64;
                            chunk
                        }
                        ActiveScriptedReply::Failed(err_val) => {
                            let err_val = err_val.clone();
                            scripted.active = ActiveScriptedReply::Closed;
                            let err_tag = find_case_tag(types, result_ty, "Error");
                            return Ok(heap.make_case(types, result_ty, err_tag, vec![err_val])?);
                        }
                        ActiveScriptedReply::Refused => {
                            scripted.active = ActiveScriptedReply::Closed;
                            let host_val = scripted.host_val.clone();
                            let port = i64::from(scripted.port);
                            return make_network_error(
                                heap,
                                types,
                                result_ty,
                                NetErr::Refused,
                                &host_val,
                                port,
                            );
                        }
                        ActiveScriptedReply::Closed => Vec::new(),
                        ActiveScriptedReply::Pending => {
                            return make_network_error(
                                heap,
                                types,
                                result_ty,
                                NetErr::TimedOut,
                                &Value::Unit,
                                0,
                            );
                        }
                    };
                    let val = heap.alloc_bytes(chunk)?;
                    make_network_ok(heap, types, result_ty, val)
                }
                ConnectionCap::Real(cell) => {
                    let mut scratch = ScratchGuard::new(heap.budget());
                    scratch.charge(most_usize)?;
                    let mut buf = vec![0u8; most_usize];
                    let mut real = cell.borrow_mut();
                    match real_receive_some(&mut real, &mut buf) {
                        Ok(n) => {
                            buf.truncate(n);
                            drop(scratch);
                            let val = heap.alloc_bytes(buf)?;
                            make_network_ok(heap, types, result_ty, val)
                        }
                        Err(err) => {
                            drop(scratch);
                            make_network_error(heap, types, result_ty, err, &Value::Unit, 0)
                        }
                    }
                }
            }
        }

        _ => unreachable!("non-network hostop routed to network module"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    fn make_ca_and_leaf(
        sans: Vec<String>,
    ) -> (
        CertificateDer<'static>,
        CertificateDer<'static>,
        rustls::pki_types::PrivateKeyDer<'static>,
    ) {
        use rcgen::{BasicConstraints, CertificateParams, IsCa, Issuer, KeyPair};
        let ca_key = KeyPair::generate().unwrap();
        let mut ca_params = CertificateParams::new(Vec::new()).unwrap();
        ca_params.is_ca = IsCa::Ca(BasicConstraints::Unconstrained);
        let ca_cert = ca_params.self_signed(&ca_key).unwrap();
        let issuer = Issuer::from_params(&ca_params, &ca_key);

        let leaf_key = KeyPair::generate().unwrap();
        let leaf_params = CertificateParams::new(sans).unwrap();
        let leaf_cert = leaf_params.signed_by(&leaf_key, &issuer).unwrap();

        let ca_der = ca_cert.der().clone();
        let leaf_der = leaf_cert.der().clone();
        let key_der = rustls::pki_types::PrivateKeyDer::Pkcs8(leaf_key.serialize_der().into());
        (ca_der, leaf_der, key_der)
    }

    #[test]
    fn pure_rust_rustls_loopback_handshake_verify_and_io() {
        let (ca_der, leaf_der, key_der) =
            make_ca_and_leaf(vec!["127.0.0.1".to_string(), "localhost".to_string()]);
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let server_cfg = Arc::new(
            rustls::ServerConfig::builder_with_provider(provider)
                .with_protocol_versions(&[&rustls::version::TLS13, &rustls::version::TLS12])
                .unwrap()
                .with_no_client_auth()
                .with_single_cert(vec![leaf_der], key_der)
                .unwrap(),
        );

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();

        let srv_cfg = Arc::clone(&server_cfg);
        let handle = std::thread::spawn(move || {
            for _ in 0..4 {
                let (mut sock, _) = listener.accept().unwrap();
                let mut conn = rustls::ServerConnection::new(Arc::clone(&srv_cfg)).unwrap();
                let mut stream = rustls::Stream::new(&mut conn, &mut sock);
                let mut req = [0u8; 64];
                if let Ok(n) = stream.read(&mut req)
                    && n > 0
                {
                    let _ = stream.write_all(&req[..n]);
                    let _ = stream.flush();
                    conn.send_close_notify();
                    let _ = conn.write_tls(&mut sock);
                }
            }
        });

        let deadline = Instant::now() + Duration::from_secs(5);

        // 1. Trusted CA + matching IP SAN succeeds and echoes data + clean close_notify EOF.
        let mut trusted_store = RootCertStore::empty();
        trusted_store.add(ca_der.clone()).unwrap();
        let trusted_store = Arc::new(trusted_store);
        let tcp = open_tcp_socket(&[addr], deadline).unwrap();
        let tls = tls_handshake_with_store(tcp, "127.0.0.1", deadline, Arc::clone(&trusted_store))
            .expect("trusted IP SAN handshake");
        let open_conns = Rc::new(Cell::new(1));
        let mut rconn = RealConnection {
            transport: RealTransport::Tls(Box::new(tls)),
            deadline,
            open_connections: Rc::clone(&open_conns),
        };
        real_send_all(&mut rconn, b"ping-tls").unwrap();
        let mut buf = [0u8; 64];
        let n = real_receive_some(&mut rconn, &mut buf).unwrap();
        assert_eq!(&buf[..n], b"ping-tls");
        let eof = real_receive_some(&mut rconn, &mut buf).unwrap();
        assert_eq!(eof, 0);
        drop(rconn);
        assert_eq!(open_conns.get(), 0);

        // 2. Trusted CA + matching DNS SAN + SNI ("localhost") succeeds.
        let tcp = open_tcp_socket(&[addr], deadline).unwrap();
        let tls = tls_handshake_with_store(tcp, "localhost", deadline, Arc::clone(&trusted_store))
            .expect("trusted DNS SAN handshake");
        let mut rconn = RealConnection {
            transport: RealTransport::Tls(Box::new(tls)),
            deadline,
            open_connections: Rc::new(Cell::new(1)),
        };
        real_send_all(&mut rconn, b"sni").unwrap();
        let n = real_receive_some(&mut rconn, &mut buf).unwrap();
        assert_eq!(&buf[..n], b"sni");
        drop(rconn);

        // 3. Untrusted CA -> Certificate error.
        let (other_ca, _, _) = make_ca_and_leaf(vec!["other.example".to_string()]);
        let mut wrong_store = RootCertStore::empty();
        wrong_store.add(other_ca).unwrap();
        let tcp = open_tcp_socket(&[addr], deadline).unwrap();
        let err = tls_handshake_with_store(tcp, "127.0.0.1", deadline, Arc::new(wrong_store))
            .err()
            .expect("untrusted cert must fail");
        assert!(matches!(err, NetErr::Certificate(_)), "got {err:?}");

        // 4. Trusted CA + wrong hostname -> Certificate error.
        let tcp = open_tcp_socket(&[addr], deadline).unwrap();
        let err =
            tls_handshake_with_store(tcp, "wrong.example", deadline, Arc::clone(&trusted_store))
                .err()
                .expect("wrong hostname must fail");
        assert!(matches!(err, NetErr::Certificate(_)), "got {err:?}");

        handle.join().unwrap();

        // 5. Plain HTTP server -> Broken; immediate TCP close -> Closed.
        let plain_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let plain_addr = plain_listener.local_addr().unwrap();
        let plain_thread = std::thread::spawn(move || {
            let (mut s1, _) = plain_listener.accept().unwrap();
            let _ = s1.write_all(b"HTTP/1.1 200 OK\r\n\r\n");
            let _ = s1.flush();
            let (s2, _) = plain_listener.accept().unwrap();
            drop(s2);
        });
        let tcp = open_tcp_socket(&[plain_addr], deadline).unwrap();
        let err = tls_handshake_with_store(tcp, "127.0.0.1", deadline, Arc::clone(&trusted_store))
            .err()
            .expect("plain HTTP must fail TLS");
        assert!(matches!(err, NetErr::Broken(_)), "got {err:?}");

        let tcp = open_tcp_socket(&[plain_addr], deadline).unwrap();
        let err = tls_handshake_with_store(tcp, "127.0.0.1", deadline, trusted_store)
            .err()
            .expect("raw close must fail TLS");
        assert!(matches!(err, NetErr::Closed), "got {err:?}");
        plain_thread.join().unwrap();

        // 6. Default root store caches cleanly across calls (returns the same Arc pointer).
        let s1 = load_root_store().expect("default root store");
        let s2 = load_root_store().expect("cached default root store");
        assert!(Arc::ptr_eq(&s1, &s2));
    }

    #[test]
    fn outbound_stream_resets_idle_timeout_per_chunk_and_aborts_on_stall_or_stop() {
        // 1. Active slow peer sending 7 chunks every 60ms (~420ms total) succeeds with 200ms idle timeout.
        let slow_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let slow_addr = slow_listener.local_addr().unwrap();
        let slow_thread = std::thread::spawn(move || {
            let (mut sock, _) = slow_listener.accept().unwrap();
            let mut req = [0u8; 32];
            let _ = sock.read(&mut req);
            for i in 0..7 {
                std::thread::sleep(Duration::from_millis(60));
                let _ = sock.write_all(format!("c{i};").as_bytes());
                let _ = sock.flush();
            }
        });

        let stop = AtomicBool::new(false);
        let mut out = OutboundStream::connect(
            "127.0.0.1",
            slow_addr.port(),
            false,
            Duration::from_secs(2),
            Duration::from_millis(200),
            Duration::from_secs(3),
        )
        .unwrap();
        out.send_all(b"hi", &stop).unwrap();
        let mut received = Vec::new();
        let mut buf = [0u8; 64];
        loop {
            let n = out.recv_some(&mut buf, &stop).unwrap();
            if n == 0 {
                break;
            }
            received.extend_from_slice(&buf[..n]);
        }
        assert_eq!(received, b"c0;c1;c2;c3;c4;c5;c6;");
        slow_thread.join().unwrap();

        // 2. Silent peer hits idle timeout promptly.
        let silent_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let silent_addr = silent_listener.local_addr().unwrap();
        let (_hold_tx, hold_rx) = std::sync::mpsc::channel::<()>();
        let silent_thread = std::thread::spawn(move || {
            let (_sock, _) = silent_listener.accept().unwrap();
            let _ = hold_rx.recv();
        });
        let mut stalled = OutboundStream::connect(
            "127.0.0.1",
            silent_addr.port(),
            false,
            Duration::from_secs(2),
            Duration::from_millis(120),
            Duration::from_secs(5),
        )
        .unwrap();
        let err = stalled.recv_some(&mut buf, &stop).unwrap_err();
        assert_eq!(err, OutboundError::TimedOut);
        drop(_hold_tx);
        silent_thread.join().unwrap();

        // 3. Setting stopping flag interrupts blocked recv_some promptly.
        let stop_listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let stop_addr = stop_listener.local_addr().unwrap();
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let stop_peer = std::thread::spawn(move || {
            let (_sock, _) = stop_listener.accept().unwrap();
            let _ = done_rx.recv();
        });
        let stop_flag = Arc::new(AtomicBool::new(false));
        let stop_trigger = Arc::clone(&stop_flag);
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(50));
            stop_trigger.store(true, Ordering::Relaxed);
        });
        let mut aborted = OutboundStream::connect(
            "127.0.0.1",
            stop_addr.port(),
            false,
            Duration::from_secs(2),
            Duration::from_secs(5),
            Duration::from_secs(10),
        )
        .unwrap();
        let t0 = Instant::now();
        let err = aborted.recv_some(&mut buf, &stop_flag).unwrap_err();
        assert_eq!(err, OutboundError::Closed);
        assert!(t0.elapsed() < Duration::from_secs(1));
        drop(done_tx);
        stop_peer.join().unwrap();
    }
}
