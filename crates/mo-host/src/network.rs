use std::cell::{Cell, RefCell};
use std::io::{self, BufReader, ErrorKind, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
#[cfg(unix)]
use std::os::fd::AsFd;
use std::path::Path;
use std::rc::Rc;
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use mo_bytecode::Capability;
use mo_bytecode::types::TypeTable;
use mo_def::HostOpId;
use mo_def::limits::{
    MO_LIMIT_FAKE_QUOTE, MO_LIMIT_HOST_NAME, MO_LIMIT_LOOKUP_ADDRESSES, MO_LIMIT_LOOKUPS,
    MO_LIMIT_OPEN_CONNECTIONS, MO_LIMIT_RECEIVE_BYTES,
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

enum NetworkCap {
    Real {
        allowed: Option<Vec<TargetRule>>,
    },
    Fake {
        peers: Value, // Map(Text, Map(Bytes, Bytes))
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
    sent: Vec<u8>,
    position: i64, // -1 before first receive
}

enum ConnectionCap {
    Real(RefCell<RealConnection>),
    Fake(RefCell<FakeConnection>),
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

// ---------------------------------------------------------------------------
// Real non-blocking socket + pure-Rust rustls TLS helpers
// ---------------------------------------------------------------------------

#[cfg(unix)]
fn wait_fd(
    fd: std::os::fd::BorrowedFd<'_>,
    events: rustix::event::PollFlags,
    deadline: Instant,
) -> Result<(), NetErr> {
    use rustix::event::{PollFd, Timespec, poll};
    use rustix::io::Errno;

    loop {
        let now = Instant::now();
        if now >= deadline {
            return Err(NetErr::TimedOut);
        }
        let rem = deadline - now;
        let ts = Timespec {
            tv_sec: i64::try_from(rem.as_secs()).unwrap_or(i64::MAX),
            tv_nsec: i64::from(rem.subsec_nanos()),
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

fn load_root_store() -> Result<RootCertStore, NetErr> {
    let mut store = RootCertStore::empty();
    let cert_file = std::env::var_os("SSL_CERT_FILE").filter(|v| !v.is_empty());
    let cert_dir = std::env::var_os("SSL_CERT_DIR").filter(|v| !v.is_empty());

    if cert_file.is_some() || cert_dir.is_some() {
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
        return Ok(store);
    }

    let native = rustls_native_certs::load_native_certs();
    let (added, _) = store.add_parsable_certificates(native.certs);
    if added == 0 {
        store.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
    }
    if store.is_empty() {
        return Err(NetErr::Certificate(
            "could not load root certificates".to_string(),
        ));
    }
    Ok(store)
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

fn tls_flush_writes(tls: &mut RealTls, deadline: Instant) -> Result<(), NetErr> {
    while tls.conn.wants_write() {
        match tls.conn.write_tls(&mut tls.stream) {
            Ok(0) => return Err(NetErr::Closed),
            Ok(_) => {}
            Err(e) if e.kind() == ErrorKind::WouldBlock => {
                #[cfg(unix)]
                wait_fd(tls.stream.as_fd(), rustix::event::PollFlags::OUT, deadline)?;
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

fn tls_handshake(stream: TcpStream, host: &str, deadline: Instant) -> Result<RealTls, NetErr> {
    let root_store = load_root_store()?;
    tls_handshake_with_store(stream, host, deadline, root_store)
}

fn tls_handshake_with_store(
    stream: TcpStream,
    host: &str,
    deadline: Instant,
    root_store: RootCertStore,
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
    if Instant::now() >= conn.deadline {
        return Err(NetErr::TimedOut);
    }
    if bytes.is_empty() {
        return Ok(());
    }
    match &mut conn.transport {
        RealTransport::Plain(stream) => {
            let mut sent = 0usize;
            while sent < bytes.len() {
                #[cfg(unix)]
                wait_fd(stream.as_fd(), rustix::event::PollFlags::OUT, conn.deadline)?;
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
                tls_flush_writes(tls, conn.deadline)?;
            }
            Ok(())
        }
    }
}

fn real_receive_some(conn: &mut RealConnection, buf: &mut [u8]) -> Result<usize, NetErr> {
    if Instant::now() >= conn.deadline {
        return Err(NetErr::TimedOut);
    }
    match &mut conn.transport {
        RealTransport::Plain(stream) => loop {
            #[cfg(unix)]
            wait_fd(stream.as_fd(), rustix::event::PollFlags::IN, conn.deadline)?;
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
                tls_flush_writes(tls, conn.deadline)?;
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
                    wait_fd(
                        tls.stream.as_fd(),
                        rustix::event::PollFlags::IN,
                        conn.deadline,
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
            allowed: None,
        }),
    )?)
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
        HostOpId::NetworkAllowHosts => {
            let net = crate::downcast_cap::<NetworkCap>(&args[0], "Network capability");
            let existing = match net {
                NetworkCap::Real { allowed } | NetworkCap::Fake { allowed, .. } => allowed,
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
                NetworkCap::Fake { peers, .. } => NetworkCap::Fake {
                    peers: peers.clone(),
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
                NetworkCap::Real { allowed } | NetworkCap::Fake { allowed, .. } => allowed,
            };
            if !is_host_allowed(allowed, host_str, port_u16) {
                return make_network_error(heap, types, result_ty, NetErr::Refused, &args[1], port);
            }

            let ok_tag = find_case_tag(types, result_ty, "Ok");
            let conn_ty = types.get(result_ty).cases[ok_tag as usize].fields[0].ty;

            match net {
                NetworkCap::Fake { peers, .. } => {
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
                        sent: Vec::new(),
                        position: -1,
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
        let tcp = open_tcp_socket(&[addr], deadline).unwrap();
        let tls = tls_handshake_with_store(tcp, "127.0.0.1", deadline, trusted_store.clone())
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
        let tls = tls_handshake_with_store(tcp, "localhost", deadline, trusted_store.clone())
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
        let err = tls_handshake_with_store(tcp, "127.0.0.1", deadline, wrong_store)
            .err()
            .expect("untrusted cert must fail");
        assert!(matches!(err, NetErr::Certificate(_)), "got {err:?}");

        // 4. Trusted CA + wrong hostname -> Certificate error.
        let tcp = open_tcp_socket(&[addr], deadline).unwrap();
        let err = tls_handshake_with_store(tcp, "wrong.example", deadline, trusted_store.clone())
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
        let err = tls_handshake_with_store(tcp, "127.0.0.1", deadline, trusted_store.clone())
            .err()
            .expect("plain HTTP must fail TLS");
        assert!(matches!(err, NetErr::Broken(_)), "got {err:?}");

        let tcp = open_tcp_socket(&[plain_addr], deadline).unwrap();
        let err = tls_handshake_with_store(tcp, "127.0.0.1", deadline, trusted_store)
            .err()
            .expect("raw close must fail TLS");
        assert!(matches!(err, NetErr::Closed), "got {err:?}");
        plain_thread.join().unwrap();
    }
}
