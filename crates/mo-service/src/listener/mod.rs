//! Nonblocking TCP and Unix domain socket listeners with dual-listener `poll` multiplexing
//! (`src/host/listener.c` parity).

pub mod credentials;
pub mod signals;

use std::io::{self, Read, Write};
use std::net::{IpAddr, Shutdown, SocketAddr, TcpListener, TcpStream};
#[cfg(unix)]
use std::os::fd::AsFd;
#[cfg(unix)]
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use mo_def::limits::{MO_LIMIT_MCP_CONNECTIONS, MO_LIMIT_MCP_TOKEN_BYTES};

use credentials::Bearer;

/// Listener operation status (`enum mo_listener_status`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ListenerStatus {
    /// Operation succeeded.
    Ok,
    /// Nonblocking operation would block; try again after `poll`.
    Again,
    /// Peer closed its write end cleanly.
    Eof,
    /// Socket/filesystem I/O error.
    Io,
    /// Memory allocation refused.
    Memory,
    /// Connection table is full (`MO_LIMIT_MCP_CONNECTIONS`).
    Full,
    /// Connection key no longer refers to a live slot.
    Stale,
    /// Invalid listener configuration (non-loopback without `--expose`, invalid address, etc.).
    Config,
    /// Missing or invalid bearer token environment variable.
    Credential,
    /// Bind address/port is already in use.
    AddressInUse,
}

impl ListenerStatus {
    /// Canonical human-readable status message (`mo_listener_status_text`).
    #[must_use]
    pub const fn text(self) -> Option<&'static str> {
        match self {
            Self::Ok => None,
            Self::Again => Some("listener operation would block"),
            Self::Eof => Some("listener peer closed"),
            Self::Io => Some("listener I/O failed"),
            Self::Memory => Some("listener memory allocation failed"),
            Self::Full => Some("listener connection table is full"),
            Self::Stale => Some("listener connection handle is stale"),
            Self::Config => Some("listener address or port configuration is invalid"),
            Self::Credential => Some("bearer token environment variable is unset or invalid"),
            Self::AddressInUse => Some("Address already in use"),
        }
    }
}

/// Bitflags for `poll` readiness (`enum mo_listener_event`).
pub const EVENT_READABLE: u32 = 1;
/// Connection socket is writable.
pub const EVENT_WRITABLE: u32 = 2;
/// Connection socket encountered hangup/error.
pub const EVENT_ERROR: u32 = 4;

/// Configuration for opening a TCP listener (`struct mo_listener_config`).
#[derive(Debug, Clone)]
pub struct ListenerConfig {
    /// IP address string (defaults to `"127.0.0.1"` when `None`).
    pub address: Option<String>,
    /// TCP port (`0` selects an ephemeral port).
    pub port: u16,
    /// Whether non-loopback addresses are allowed.
    pub expose: bool,
}

/// Stable connection handle (`struct mo_listener_key`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ConnectionKey {
    /// Slot index in the listener's connection table.
    pub slot: usize,
    /// Monotonic generation counter preventing ABA reuse.
    pub generation: u64,
}

/// Monotonic and wall-clock timestamps in microseconds (`struct mo_listener_time`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ListenerTime {
    /// Microseconds since listener epoch.
    pub monotonic_us: u64,
    /// Microseconds since Unix epoch.
    pub wall_us: i64,
}

/// Readiness interest specification for one connection.
#[derive(Debug, Clone, Copy)]
pub struct Interest {
    /// Target connection handle.
    pub key: ConnectionKey,
    /// Requested event bitmask (`EVENT_READABLE | EVENT_WRITABLE`).
    pub events: u32,
}

/// Input to `wait` / `wait_pair`.
#[derive(Debug, Clone)]
pub struct WaitRequest {
    /// Monotonic deadline in microseconds (`u64::MAX` for no deadline).
    pub until_us: u64,
    /// Whether to poll the listening socket for incoming connections.
    pub accept: bool,
    /// Connection interests to poll.
    pub interests: Vec<Interest>,
}

/// Readiness result returned from `wait` / `wait_pair`.
#[derive(Debug, Clone)]
pub struct WaitReady {
    /// Timestamp sampled immediately after `poll`.
    pub now: ListenerTime,
    /// Whether the listening socket has a pending connection.
    pub accept: bool,
    /// Event bitmask corresponding 1-to-1 with `WaitRequest::interests`.
    pub events: Vec<u32>,
}

enum ListenerSocket {
    Tcp(TcpListener),
    #[cfg(unix)]
    Unix(UnixListener),
}

enum ConnStream {
    Tcp(TcpStream),
    #[cfg(unix)]
    Unix(UnixStream),
}

impl ConnStream {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            Self::Tcp(s) => s.read(buf),
            #[cfg(unix)]
            Self::Unix(s) => s.read(buf),
        }
    }

    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            Self::Tcp(s) => s.write(buf),
            #[cfg(unix)]
            Self::Unix(s) => s.write(buf),
        }
    }

    fn shutdown_write(&mut self) -> io::Result<()> {
        match self {
            Self::Tcp(s) => s.shutdown(Shutdown::Write),
            #[cfg(unix)]
            Self::Unix(s) => s.shutdown(Shutdown::Write),
        }
    }

    #[cfg(unix)]
    fn as_borrowed_fd(&self) -> std::os::fd::BorrowedFd<'_> {
        match self {
            Self::Tcp(s) => s.as_fd(),
            Self::Unix(s) => s.as_fd(),
        }
    }
}

struct Slot {
    stream: Option<ConnStream>,
    generation: u64,
}

/// Nonblocking TCP or Unix domain socket listener with bounded connection table.
pub struct Listener {
    socket: Option<ListenerSocket>,
    #[cfg(unix)]
    unix_path: Option<PathBuf>,
    port: u16,
    bearer: Option<Bearer>,
    is_admin: bool,
    epoch: Instant,
    slots: Vec<Slot>,
    next_generation: u64,
}

impl Drop for Listener {
    fn drop(&mut self) {
        self.socket = None;
        #[cfg(unix)]
        if let Some(path) = self.unix_path.take() {
            let _ = std::fs::remove_file(path);
        }
    }
}

/// Checks if an IP address is loopback (`127.0.0.0/8` or `::1`).
#[must_use]
pub fn is_loopback_ip(ip: IpAddr) -> bool {
    ip.is_loopback()
}

/// Validates and parses a bind address string, enforcing `--expose` for non-loopback addresses.
pub fn parse_bind_ip(address: Option<&str>, expose: bool) -> Result<IpAddr, ListenerStatus> {
    let text = address.unwrap_or("127.0.0.1");
    let ip: IpAddr = text.parse().map_err(|_| ListenerStatus::Config)?;
    if !expose && !is_loopback_ip(ip) {
        return Err(ListenerStatus::Config);
    }
    Ok(ip)
}

impl Listener {
    fn new_slots() -> Vec<Slot> {
        let mut slots = Vec::with_capacity(MO_LIMIT_MCP_CONNECTIONS as usize);
        for _ in 0..MO_LIMIT_MCP_CONNECTIONS {
            slots.push(Slot {
                stream: None,
                generation: 0,
            });
        }
        slots
    }

    /// Opens a TCP listener with bearer authentication from `token_env` (`mo_listener_open`).
    pub fn open_tcp(
        config: &ListenerConfig,
        token_env: Option<&str>,
    ) -> Result<Self, ListenerStatus> {
        let ip = parse_bind_ip(config.address.as_deref(), config.expose)?;
        let bearer = match token_env {
            Some(env_name) => Some(Bearer::from_env(env_name).ok_or(ListenerStatus::Credential)?),
            None => None,
        };
        let addr = SocketAddr::new(ip, config.port);
        let tcp = TcpListener::bind(addr).map_err(|e| {
            if e.kind() == io::ErrorKind::AddrInUse {
                ListenerStatus::AddressInUse
            } else {
                ListenerStatus::Io
            }
        })?;
        tcp.set_nonblocking(true).map_err(|_| ListenerStatus::Io)?;
        let bound_port = tcp.local_addr().map_err(|_| ListenerStatus::Io)?.port();
        Ok(Self {
            socket: Some(ListenerSocket::Tcp(tcp)),
            #[cfg(unix)]
            unix_path: None,
            port: bound_port,
            bearer,
            is_admin: false,
            epoch: Instant::now(),
            slots: Self::new_slots(),
            next_generation: 1,
        })
    }

    /// Opens a `0600` Unix domain socket admin listener (`mo_listener_open_unix`).
    ///
    /// Probes an existing socket path to distinguish an active listener (refused with `Io`)
    /// from a stale socket file left after `SIGKILL` (unlinked and replaced).
    #[cfg(unix)]
    pub fn open_unix(path: &Path) -> Result<Self, ListenerStatus> {
        #[cfg(target_os = "linux")]
        const UNIX_PATH_LIMIT: usize = 108;
        #[cfg(not(target_os = "linux"))]
        const UNIX_PATH_LIMIT: usize = 104;
        let raw = path.as_os_str();
        if raw.is_empty() || raw.len() >= UNIX_PATH_LIMIT {
            return Err(ListenerStatus::Config);
        }
        if let Ok(meta) = std::fs::symlink_metadata(path) {
            use std::os::unix::fs::FileTypeExt;
            if !meta.file_type().is_socket() {
                return Err(ListenerStatus::Io);
            }
            // Probe whether a live server is still listening on this socket.
            match UnixStream::connect(path) {
                Ok(_) => return Err(ListenerStatus::Io),
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::ConnectionRefused | io::ErrorKind::NotFound
                    ) =>
                {
                    let _ = std::fs::remove_file(path);
                }
                Err(_) => return Err(ListenerStatus::Io),
            }
        }

        let prev_mask = rustix::process::umask(rustix::fs::Mode::from_bits_truncate(0o177));
        let bind_res = UnixListener::bind(path);
        let _ = rustix::process::umask(prev_mask);
        let unix = bind_res.map_err(|_| ListenerStatus::Io)?;
        // Enforce 0600 permissions explicitly regardless of prior umask.
        let _ = rustix::fs::chmod(path, rustix::fs::Mode::from_bits_truncate(0o600));
        unix.set_nonblocking(true).map_err(|_| ListenerStatus::Io)?;

        Ok(Self {
            socket: Some(ListenerSocket::Unix(unix)),
            unix_path: Some(path.to_path_buf()),
            port: 0,
            bearer: None,
            is_admin: true,
            epoch: Instant::now(),
            slots: Self::new_slots(),
            next_generation: 1,
        })
    }

    /// Returns the bound TCP port (`0` for Unix domain listeners).
    #[must_use]
    pub fn port(&self) -> u16 {
        self.port
    }

    /// Returns `true` if this is an admin Unix socket listener.
    #[must_use]
    pub fn is_admin(&self) -> bool {
        self.is_admin
    }

    /// Returns current monotonic and wall-clock timestamps in microseconds.
    #[must_use]
    pub fn now(&self) -> ListenerTime {
        let monotonic_us = self.epoch.elapsed().as_micros() as u64;
        let wall_us = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0i64, |d| d.as_micros() as i64);
        ListenerTime {
            monotonic_us,
            wall_us,
        }
    }

    /// Stops accepting new connections by closing the listening socket while keeping
    /// active connections open to drain (`mo_listener_stop_accepting`).
    pub fn stop_accepting(&mut self) {
        self.socket = None;
        #[cfg(unix)]
        if let Some(path) = self.unix_path.take() {
            let _ = std::fs::remove_file(path);
        }
    }

    /// Authenticates a bearer token slice (`Authorization: Bearer <token>` slice).
    #[must_use]
    pub fn authenticate(&self, token_bytes: &[u8]) -> bool {
        if self.is_admin {
            return true;
        }
        let Some(ref bearer) = self.bearer else {
            return true;
        };
        if token_bytes.len() as u64 > MO_LIMIT_MCP_TOKEN_BYTES {
            return false;
        }
        bearer.check(token_bytes)
    }

    /// Accepts one pending connection if a slot is available.
    pub fn accept(&mut self) -> Result<ConnectionKey, ListenerStatus> {
        let Some(slot_idx) = self.slots.iter().position(|s| s.stream.is_none()) else {
            return Err(ListenerStatus::Full);
        };
        let Some(ref sock) = self.socket else {
            return Err(ListenerStatus::Again);
        };
        let stream = match sock {
            ListenerSocket::Tcp(l) => match l.accept() {
                Ok((s, _)) => {
                    let _ = s.set_nonblocking(true);
                    let _ = s.set_nodelay(true);
                    ConnStream::Tcp(s)
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    return Err(ListenerStatus::Again);
                }
                Err(_) => return Err(ListenerStatus::Io),
            },
            #[cfg(unix)]
            ListenerSocket::Unix(listener) => match listener.accept() {
                Ok((s, _)) => {
                    let _ = s.set_nonblocking(true);
                    ConnStream::Unix(s)
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    return Err(ListenerStatus::Again);
                }
                Err(_) => return Err(ListenerStatus::Io),
            },
        };
        let generation = self.next_generation;
        self.next_generation = self.next_generation.wrapping_add(1).max(1);
        self.slots[slot_idx] = Slot {
            stream: Some(stream),
            generation,
        };
        Ok(ConnectionKey {
            slot: slot_idx,
            generation,
        })
    }

    fn slot_mut(&mut self, key: ConnectionKey) -> Result<&mut ConnStream, ListenerStatus> {
        let slot = self.slots.get_mut(key.slot).ok_or(ListenerStatus::Stale)?;
        if slot.generation != key.generation {
            return Err(ListenerStatus::Stale);
        }
        slot.stream.as_mut().ok_or(ListenerStatus::Stale)
    }

    /// Nonblocking read from a connection.
    pub fn read(&mut self, key: ConnectionKey, buf: &mut [u8]) -> Result<usize, ListenerStatus> {
        let stream = self.slot_mut(key)?;
        match stream.read(buf) {
            Ok(0) => Err(ListenerStatus::Eof),
            Ok(n) => Ok(n),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => Err(ListenerStatus::Again),
            Err(e) if e.kind() == io::ErrorKind::Interrupted => Err(ListenerStatus::Again),
            Err(_) => Err(ListenerStatus::Io),
        }
    }

    /// Nonblocking write to a connection.
    pub fn write(&mut self, key: ConnectionKey, buf: &[u8]) -> Result<usize, ListenerStatus> {
        let stream = self.slot_mut(key)?;
        match stream.write(buf) {
            Ok(0) => Err(ListenerStatus::Io),
            Ok(n) => Ok(n),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => Err(ListenerStatus::Again),
            Err(e) if e.kind() == io::ErrorKind::Interrupted => Err(ListenerStatus::Again),
            Err(_) => Err(ListenerStatus::Io),
        }
    }

    /// Half-closes the write side of a connection (`shutdown(SHUT_WR)`).
    pub fn half_close(&mut self, key: ConnectionKey) -> Result<(), ListenerStatus> {
        let stream = self.slot_mut(key)?;
        let _ = stream.shutdown_write();
        Ok(())
    }

    /// Closes a connection and frees its slot.
    pub fn close_connection(&mut self, key: ConnectionKey) {
        if let Some(slot) = self.slots.get_mut(key.slot)
            && slot.generation == key.generation
        {
            slot.stream = None;
        }
    }

    /// Returns the number of currently active connections.
    #[must_use]
    pub fn active_connections(&self) -> usize {
        self.slots.iter().filter(|s| s.stream.is_some()).count()
    }

    #[cfg(unix)]
    fn listener_fd(&self) -> Option<std::os::fd::BorrowedFd<'_>> {
        match self.socket.as_ref()? {
            ListenerSocket::Tcp(l) => Some(l.as_fd()),
            ListenerSocket::Unix(listener) => Some(listener.as_fd()),
        }
    }

    #[cfg(unix)]
    fn connection_fd(&self, key: ConnectionKey) -> Option<std::os::fd::BorrowedFd<'_>> {
        let slot = self.slots.get(key.slot)?;
        if slot.generation != key.generation {
            return None;
        }
        slot.stream.as_ref().map(ConnStream::as_borrowed_fd)
    }
}

/// Polls a primary listener and an optional admin listener simultaneously
/// (`mo_listener_wait_pair`).
#[cfg(unix)]
pub fn wait_pair(
    primary: &Listener,
    primary_req: &WaitRequest,
    admin: Option<(&Listener, &WaitRequest)>,
) -> Result<(WaitReady, Option<WaitReady>), ListenerStatus> {
    use rustix::event::{PollFd, PollFlags, Timespec, poll};

    let now = primary.now();
    let deadline = match admin {
        Some((_, ar)) => primary_req.until_us.min(ar.until_us),
        None => primary_req.until_us,
    };

    let mut pfds: Vec<PollFd<'_>> = Vec::with_capacity(
        2 + primary_req.interests.len() + admin.map_or(0, |(_, r)| r.interests.len()),
    );

    let mut primary_accept_idx = None;
    if primary_req.accept
        && let Some(fd) = primary.listener_fd()
    {
        primary_accept_idx = Some(pfds.len());
        pfds.push(PollFd::from_borrowed_fd(fd, PollFlags::IN));
    }
    let mut primary_conn_idx: Vec<Option<usize>> = Vec::with_capacity(primary_req.interests.len());
    for interest in &primary_req.interests {
        if let Some(fd) = primary.connection_fd(interest.key) {
            let mut flags = PollFlags::empty();
            if interest.events & EVENT_READABLE != 0 {
                flags |= PollFlags::IN;
            }
            if interest.events & EVENT_WRITABLE != 0 {
                flags |= PollFlags::OUT;
            }
            primary_conn_idx.push(Some(pfds.len()));
            pfds.push(PollFd::from_borrowed_fd(fd, flags));
        } else {
            primary_conn_idx.push(None);
        }
    }

    let mut admin_accept_idx = None;
    let mut admin_conn_idx: Vec<Option<usize>> = Vec::new();
    if let Some((adm, areq)) = admin {
        if areq.accept
            && let Some(fd) = adm.listener_fd()
        {
            admin_accept_idx = Some(pfds.len());
            pfds.push(PollFd::from_borrowed_fd(fd, PollFlags::IN));
        }
        admin_conn_idx.reserve(areq.interests.len());
        for interest in &areq.interests {
            if let Some(fd) = adm.connection_fd(interest.key) {
                let mut flags = PollFlags::empty();
                if interest.events & EVENT_READABLE != 0 {
                    flags |= PollFlags::IN;
                }
                if interest.events & EVENT_WRITABLE != 0 {
                    flags |= PollFlags::OUT;
                }
                admin_conn_idx.push(Some(pfds.len()));
                pfds.push(PollFd::from_borrowed_fd(fd, flags));
            } else {
                admin_conn_idx.push(None);
            }
        }
    }

    let wait_us = if deadline == u64::MAX {
        50_000u64 // Cap poll sleep at 50ms so signal checks stay responsive.
    } else {
        deadline.saturating_sub(now.monotonic_us).min(50_000)
    };
    let ts = Timespec {
        tv_sec: (wait_us / 1_000_000) as i64,
        tv_nsec: ((wait_us % 1_000_000) * 1_000) as i64,
    };

    if let Err(err) = poll(&mut pfds, Some(&ts))
        && err != rustix::io::Errno::INTR
    {
        return Err(ListenerStatus::Io);
    }

    let after = primary.now();
    let decode_revents = |idx: Option<usize>| -> u32 {
        let Some(i) = idx else {
            return EVENT_ERROR;
        };
        let rev = pfds[i].revents();
        let mut bits = 0u32;
        if rev.contains(PollFlags::IN) {
            bits |= EVENT_READABLE;
        }
        if rev.contains(PollFlags::OUT) {
            bits |= EVENT_WRITABLE;
        }
        if rev.intersects(PollFlags::ERR | PollFlags::HUP | PollFlags::NVAL) {
            bits |= EVENT_ERROR;
        }
        bits
    };

    let primary_ready = WaitReady {
        now: after,
        accept: primary_accept_idx.is_some_and(|i| pfds[i].revents().contains(PollFlags::IN)),
        events: primary_conn_idx.into_iter().map(decode_revents).collect(),
    };
    let admin_ready = admin.map(|(adm, _)| WaitReady {
        now: adm.now(),
        accept: admin_accept_idx.is_some_and(|i| pfds[i].revents().contains(PollFlags::IN)),
        events: admin_conn_idx.into_iter().map(decode_revents).collect(),
    });
    Ok((primary_ready, admin_ready))
}
