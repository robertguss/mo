#![forbid(unsafe_code)]

//! Bounded in-process Server-Sent Events (SSE) broker for multi-worker `mo serve` (`api` blocks).
//!
//! Design invariants:
//! - Runs all open SSE TCP streams on a single dedicated `rustix::event::poll` background thread
//!   (`mo-api-sse`) with non-blocking sockets so open SSE tabs never starve the bounded HTTP
//!   worker pool (`MO_LIMIT_API_WORKERS`).
//! - Any HTTP worker (or background job) can call [`SseBroker::publish`] across workers; events
//!   receive a monotonic sequence `id` (`1..`) and are buffered per topic up to
//!   [`MO_LIMIT_API_SSE_HISTORY`] so reconnecting `EventSource` clients sending `Last-Event-ID`
//!   catch up cleanly or receive `event: resync_required` if history was evicted or restarted.
//! - Idle connections emit `: ping\n\n` chunked keepalives at `keepalive_interval`
//!   ([`MO_LIMIT_API_SSE_KEEPALIVE_US`] in production).

use std::collections::{HashMap, VecDeque};
use std::io::Read as _;
use std::net::TcpStream;
use std::os::fd::{AsFd, OwnedFd};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use mo_def::limits::{
    MO_LIMIT_API_SSE_CLIENT_QUEUE, MO_LIMIT_API_SSE_CLIENTS, MO_LIMIT_API_SSE_EVENT_BYTES,
    MO_LIMIT_API_SSE_HISTORY, MO_LIMIT_API_SSE_KEEPALIVE_US, MO_LIMIT_API_SSE_PUBLISH_PER_RESPONSE,
    MO_LIMIT_API_SSE_TOPICS, MO_LIMIT_API_SSE_TOPICS_PER_CLIENT,
};

#[cfg(unix)]
use rustix::event::{PollFd, PollFlags, Timespec, poll};
#[cfg(unix)]
use rustix::io::Errno;
#[cfg(unix)]
use rustix::net::{SendFlags, send};
#[cfg(unix)]
use rustix::pipe::{PipeFlags, pipe_with};

/// Internal response header stripped by `mo serve` to upgrade a `200 OK` response into a live SSE
/// subscription managed by [`SseBroker`].
pub(crate) const SSE_SUBSCRIBE_HEADER: &str = "x-mo-sse-subscribe";

/// Internal repeatable response header stripped by `mo serve` to broadcast an SSE event across all
/// open matching topic subscribers.
pub(crate) const SSE_PUBLISH_HEADER: &str = "x-mo-sse-publish";

const MAX_TOPIC_BYTES: usize = 256;
const MAX_EVENT_NAME_BYTES: usize = 256;
const MAX_JSON_DEPTH: usize = 16;
const MAX_CLIENT_WRITE_BUF_BYTES: usize = (MO_LIMIT_API_SSE_EVENT_BYTES as usize) * 8;

/// Parsed `X-Mo-Sse-Subscribe` directive extracted from an `api` route handler response.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct SseSubscribeSpec {
    pub topics: Vec<String>,
    pub last_event_id: Option<String>,
}

/// Parsed `X-Mo-Sse-Publish` directive extracted from an `api` route handler response.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct SsePublishSpec {
    pub topic: String,
    pub event: Option<String>,
    pub data: String,
}

fn sanitize_single_line(raw: &str) -> String {
    raw.chars()
        .filter(|&ch| ch != '\r' && ch != '\n' && ch != '\0')
        .collect()
}

/// Parses `{"topics":[...],"last_event_id":<string|null>}` from an internal `X-Mo-Sse-Subscribe`
/// header value.
pub(crate) fn parse_subscribe_header(raw: &str) -> Option<SseSubscribeSpec> {
    let JsonValue::Object(obj) = parse_json(raw)? else {
        return None;
    };
    let mut topics = Vec::new();
    let Some(JsonValue::Array(arr)) = find_json_field(&obj, "topics") else {
        return None;
    };
    for item in arr {
        let JsonValue::String(t) = item else {
            continue;
        };
        let clean = sanitize_single_line(t).trim().to_string();
        if clean.is_empty() || clean.len() > MAX_TOPIC_BYTES {
            continue;
        }
        if !topics.iter().any(|existing| existing == &clean) {
            topics.push(clean);
            if topics.len() >= MO_LIMIT_API_SSE_TOPICS_PER_CLIENT as usize {
                break;
            }
        }
    }
    if topics.is_empty() {
        return None;
    }

    let last_event_id = match find_json_field(&obj, "last_event_id") {
        Some(JsonValue::String(s)) => {
            let clean = sanitize_single_line(s).trim().to_string();
            if clean.is_empty() { None } else { Some(clean) }
        }
        _ => None,
    };

    Some(SseSubscribeSpec {
        topics,
        last_event_id,
    })
}

/// Parses `{"topic":"...","event":<string|null>,"data":"..."}` from an internal
/// `X-Mo-Sse-Publish` header value.
pub(crate) fn parse_publish_header(raw: &str) -> Option<SsePublishSpec> {
    let JsonValue::Object(obj) = parse_json(raw)? else {
        return None;
    };
    let Some(JsonValue::String(topic_raw)) = find_json_field(&obj, "topic") else {
        return None;
    };
    let topic = sanitize_single_line(topic_raw).trim().to_string();
    if topic.is_empty() || topic.len() > MAX_TOPIC_BYTES {
        return None;
    }

    let event = match find_json_field(&obj, "event") {
        Some(JsonValue::String(ev_raw)) => {
            let clean = sanitize_single_line(ev_raw).trim().to_string();
            if clean.is_empty() || clean.len() > MAX_EVENT_NAME_BYTES {
                None
            } else {
                Some(clean)
            }
        }
        Some(JsonValue::Null) | None => None,
        Some(_) => return None,
    };

    let Some(JsonValue::String(data_raw)) = find_json_field(&obj, "data") else {
        return None;
    };
    if data_raw.len() > MO_LIMIT_API_SSE_EVENT_BYTES as usize {
        return None;
    }

    Some(SsePublishSpec {
        topic,
        event,
        data: data_raw.clone(),
    })
}

/// Encodes `payload` as a single HTTP/1.1 `Transfer-Encoding: chunked` frame.
#[must_use]
pub(crate) fn encode_http_chunk(payload: &[u8]) -> Vec<u8> {
    if payload.is_empty() {
        return Vec::new();
    }
    let header = format!("{:x}\r\n", payload.len());
    let mut out = Vec::with_capacity(header.len() + payload.len() + 2);
    out.extend_from_slice(header.as_bytes());
    out.extend_from_slice(payload);
    out.extend_from_slice(b"\r\n");
    out
}

/// Formats one WHATWG Server-Sent Event frame with optional numeric `id:` and `event:`.
pub(crate) fn format_sse_event(seq: Option<u64>, event: Option<&str>, data: &str) -> Vec<u8> {
    use std::fmt::Write as _;

    let mut frame = String::new();
    if let Some(id) = seq {
        let _ = writeln!(&mut frame, "id: {id}");
    }
    if let Some(ev) = event {
        let clean = sanitize_single_line(ev);
        if !clean.is_empty() {
            let _ = writeln!(&mut frame, "event: {clean}");
        }
    }
    let normalized = data.replace("\r\n", "\n").replace('\r', "\n");
    for line in normalized.split('\n') {
        frame.push_str("data: ");
        frame.push_str(line);
        frame.push('\n');
    }
    frame.push('\n');
    frame.into_bytes()
}

#[derive(Debug, Clone)]
struct BrokerEvent {
    seq: u64,
    event: Option<String>,
    data: String,
}

#[derive(Debug, Default)]
struct TopicHistory {
    evicted_through: u64,
    events: VecDeque<BrokerEvent>,
    last_touched: u64,
}

struct SubscriberMailbox {
    topics: Vec<String>,
    queue: VecDeque<Vec<u8>>,
    closed: bool,
}

struct BrokerState {
    next_seq: u64,
    next_client_id: u64,
    touch_clock: u64,
    histories: HashMap<String, TopicHistory>,
    subscribers: HashMap<u64, SubscriberMailbox>,
    new_streams: Vec<(u64, TcpStream)>,
    stopping: bool,
    topic_capacity: usize,
    history_capacity: usize,
    client_capacity: usize,
}

impl BrokerState {
    fn new(topic_capacity: usize, history_capacity: usize, client_capacity: usize) -> Self {
        Self {
            next_seq: 1,
            next_client_id: 1,
            touch_clock: 1,
            histories: HashMap::new(),
            subscribers: HashMap::new(),
            new_streams: Vec::new(),
            stopping: false,
            topic_capacity: topic_capacity.max(1),
            history_capacity: history_capacity.max(1),
            client_capacity: client_capacity.max(1),
        }
    }

    fn collect_catchup_chunks(
        &self,
        topics: &[String],
        last_event_id: Option<&str>,
    ) -> Vec<Vec<u8>> {
        let Some(raw_cursor) = last_event_id else {
            return Vec::new();
        };
        let Ok(cursor) = raw_cursor.trim().parse::<u64>() else {
            return Vec::new();
        };
        let highest_issued = self.next_seq.saturating_sub(1);
        if cursor > highest_issued {
            let payload = format!("{{\"reason\":\"cursor_ahead\",\"last_event_id\":{cursor}}}");
            let frame = format_sse_event(None, Some("resync_required"), &payload);
            return vec![encode_http_chunk(&frame)];
        }

        let mut evicted = false;
        let mut replay: Vec<&BrokerEvent> = Vec::new();
        for topic in topics {
            if let Some(hist) = self.histories.get(topic) {
                if hist.evicted_through > cursor {
                    evicted = true;
                }
                for ev in &hist.events {
                    if ev.seq > cursor {
                        replay.push(ev);
                    }
                }
            }
        }
        replay.sort_by_key(|ev| ev.seq);
        replay.dedup_by_key(|ev| ev.seq);

        let mut chunks = Vec::with_capacity(replay.len() + usize::from(evicted));
        if evicted {
            let payload = format!("{{\"reason\":\"history_evicted\",\"last_event_id\":{cursor}}}");
            let frame = format_sse_event(None, Some("resync_required"), &payload);
            chunks.push(encode_http_chunk(&frame));
        }
        for ev in replay {
            let frame = format_sse_event(Some(ev.seq), ev.event.as_deref(), &ev.data);
            chunks.push(encode_http_chunk(&frame));
        }
        chunks
    }

    fn evict_lru_topic_if_full(&mut self, incoming_topic: &str) {
        if self.histories.contains_key(incoming_topic) || self.histories.len() < self.topic_capacity
        {
            return;
        }
        let candidate = self
            .histories
            .iter()
            .filter(|(topic, _)| {
                !self
                    .subscribers
                    .values()
                    .any(|sub| !sub.closed && sub.topics.iter().any(|t| t == *topic))
            })
            .min_by_key(|(_, hist)| hist.last_touched)
            .map(|(topic, _)| topic.clone())
            .or_else(|| {
                self.histories
                    .iter()
                    .min_by_key(|(_, hist)| hist.last_touched)
                    .map(|(topic, _)| topic.clone())
            });
        if let Some(victim) = candidate {
            self.histories.remove(&victim);
        }
    }
}

/// Multi-worker Server-Sent Events pub/sub broker backed by a single non-blocking poll thread.
#[cfg(unix)]
pub(crate) struct SseBroker {
    state: Arc<Mutex<BrokerState>>,
    wake_write: OwnedFd,
    driver: Mutex<Option<JoinHandle<()>>>,
}

#[cfg(unix)]
impl SseBroker {
    /// Creates an [`SseBroker`] using `mo_def::limits` bounds and keepalive interval.
    pub(crate) fn default_broker() -> std::io::Result<Self> {
        Self::with_limits(
            Duration::from_micros(MO_LIMIT_API_SSE_KEEPALIVE_US),
            MO_LIMIT_API_SSE_TOPICS as usize,
            MO_LIMIT_API_SSE_HISTORY as usize,
            MO_LIMIT_API_SSE_CLIENTS as usize,
        )
    }

    /// Creates an [`SseBroker`] with explicit keepalive and buffer bounds (used in tests).
    pub(crate) fn with_limits(
        keepalive_interval: Duration,
        topic_capacity: usize,
        history_capacity: usize,
        client_capacity: usize,
    ) -> std::io::Result<Self> {
        let (wake_read, wake_write) = pipe_with(PipeFlags::NONBLOCK | PipeFlags::CLOEXEC)
            .map_err(|e| std::io::Error::from_raw_os_error(e.raw_os_error()))?;
        let state = Arc::new(Mutex::new(BrokerState::new(
            topic_capacity,
            history_capacity,
            client_capacity,
        )));
        let thread_state = Arc::clone(&state);
        let handle = thread::Builder::new()
            .name("mo-api-sse".to_string())
            .stack_size(2 << 20)
            .spawn(move || sse_driver_loop(thread_state, wake_read, keepalive_interval))?;
        Ok(Self {
            state,
            wake_write,
            driver: Mutex::new(Some(handle)),
        })
    }

    fn wake(&self) {
        let _ = rustix::io::write(&self.wake_write, &[1u8]);
    }

    /// Registers an upgraded SSE TCP connection with initial HTTP response wire bytes and catchup
    /// replay based on `spec.last_event_id`. Returns `Err(stream)` if shutting down or at capacity.
    pub(crate) fn register_client(
        &self,
        stream: TcpStream,
        initial_wire_bytes: Vec<u8>,
        spec: SseSubscribeSpec,
    ) -> Result<(), TcpStream> {
        {
            let mut st = self
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if st.stopping || st.subscribers.len() >= st.client_capacity {
                return Err(stream);
            }
            let client_id = st.next_client_id;
            st.next_client_id = st.next_client_id.saturating_add(1);
            let catchup = st.collect_catchup_chunks(&spec.topics, spec.last_event_id.as_deref());
            let mut queue = VecDeque::with_capacity(1 + catchup.len());
            if !initial_wire_bytes.is_empty() {
                queue.push_back(initial_wire_bytes);
            }
            for chunk in catchup {
                queue.push_back(chunk);
            }
            st.subscribers.insert(
                client_id,
                SubscriberMailbox {
                    topics: spec.topics,
                    queue,
                    closed: false,
                },
            );
            st.new_streams.push((client_id, stream));
        }
        self.wake();
        Ok(())
    }

    /// Broadcasts `events` atomically into per-topic history ring buffers and active subscriber
    /// queues, waking the SSE poll thread once if any subscriber matched.
    pub(crate) fn publish(&self, events: &[SsePublishSpec]) {
        if events.is_empty() {
            return;
        }
        let mut woke_any = false;
        {
            let mut st = self
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if st.stopping {
                return;
            }
            let history_cap = st.history_capacity;
            for ev in events
                .iter()
                .take(MO_LIMIT_API_SSE_PUBLISH_PER_RESPONSE as usize)
            {
                let seq = st.next_seq;
                st.next_seq = st.next_seq.saturating_add(1);
                let touch = st.touch_clock;
                st.touch_clock = st.touch_clock.saturating_add(1);
                st.evict_lru_topic_if_full(&ev.topic);

                let hist = st.histories.entry(ev.topic.clone()).or_default();
                hist.last_touched = touch;
                hist.events.push_back(BrokerEvent {
                    seq,
                    event: ev.event.clone(),
                    data: ev.data.clone(),
                });
                while hist.events.len() > history_cap {
                    if let Some(old) = hist.events.pop_front() {
                        hist.evicted_through = hist.evicted_through.max(old.seq);
                    }
                }

                let frame = format_sse_event(Some(seq), ev.event.as_deref(), &ev.data);
                let chunk = encode_http_chunk(&frame);
                for sub in st.subscribers.values_mut() {
                    if !sub.closed && sub.topics.iter().any(|t| t == &ev.topic) {
                        if sub.queue.len() < MO_LIMIT_API_SSE_CLIENT_QUEUE as usize {
                            sub.queue.push_back(chunk.clone());
                        } else {
                            // Evict stalled slow consumer without blocking workers or fast tabs.
                            sub.closed = true;
                        }
                        woke_any = true;
                    }
                }
            }
        }
        if woke_any {
            self.wake();
        }
    }

    /// Returns the number of currently registered live SSE subscribers.
    #[cfg(test)]
    pub(crate) fn active_client_count(&self) -> usize {
        self.state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .subscribers
            .len()
    }

    /// Signals the background SSE poll thread to close all connections and joins it cleanly.
    pub(crate) fn shutdown(&self) {
        {
            let mut st = self
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            st.stopping = true;
        }
        self.wake();
        let handle = self
            .driver
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .take();
        if let Some(h) = handle {
            let _ = h.join();
        }
    }
}

#[cfg(unix)]
impl Drop for SseBroker {
    fn drop(&mut self) {
        self.shutdown();
    }
}

#[cfg(unix)]
struct ActiveStream {
    id: u64,
    stream: TcpStream,
    write_buf: Vec<u8>,
    write_offset: usize,
    last_activity: Instant,
    dead: bool,
}

#[cfg(unix)]
fn drain_wake_pipe(wake_read: &OwnedFd) {
    let mut discard = [0u8; 64];
    loop {
        match rustix::io::read(wake_read, &mut discard) {
            Ok(0) => break,
            Ok(_) => continue,
            Err(Errno::INTR) => continue,
            Err(_) => break,
        }
    }
}

#[cfg(unix)]
fn flush_active_stream(active: &mut ActiveStream) {
    while !active.dead && active.write_offset < active.write_buf.len() {
        match send(
            &active.stream,
            &active.write_buf[active.write_offset..],
            SendFlags::NOSIGNAL,
        ) {
            Ok(0) => {
                active.dead = true;
            }
            Ok(n) => {
                active.write_offset += n;
                active.last_activity = Instant::now();
                if active.write_offset >= active.write_buf.len() {
                    active.write_buf.clear();
                    active.write_offset = 0;
                    break;
                }
            }
            Err(Errno::INTR) => continue,
            Err(e) if e == Errno::AGAIN || e == Errno::WOULDBLOCK => break,
            Err(_) => {
                active.dead = true;
            }
        }
    }
}

#[cfg(unix)]
fn sse_driver_loop(
    state: Arc<Mutex<BrokerState>>,
    wake_read: OwnedFd,
    keepalive_interval: Duration,
) {
    let ping_chunk = encode_http_chunk(b": ping\n\n");
    let mut active: Vec<ActiveStream> = Vec::new();

    loop {
        let mut dead_ids: Vec<u64> = Vec::new();
        {
            let mut st = state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            if st.stopping {
                st.subscribers.clear();
                st.new_streams.clear();
                drop(st);
                for a in &mut active {
                    if a.write_offset == a.write_buf.len() {
                        let _ = send(&a.stream, b"0\r\n\r\n", SendFlags::NOSIGNAL);
                    }
                }
                active.clear();
                return;
            }

            for (id, stream) in st.new_streams.drain(..) {
                let _ = stream.set_nonblocking(true);
                let _ = stream.set_nodelay(true);
                active.push(ActiveStream {
                    id,
                    stream,
                    write_buf: Vec::new(),
                    write_offset: 0,
                    last_activity: Instant::now(),
                    dead: false,
                });
            }

            for a in &mut active {
                if let Some(sub) = st.subscribers.get_mut(&a.id) {
                    if sub.closed {
                        a.dead = true;
                        continue;
                    }
                    if !sub.queue.is_empty() {
                        if a.write_offset >= a.write_buf.len() {
                            a.write_buf.clear();
                            a.write_offset = 0;
                        } else if a.write_offset > 0
                            && a.write_buf.len() > (MO_LIMIT_API_SSE_EVENT_BYTES as usize) * 4
                        {
                            a.write_buf.drain(..a.write_offset);
                            a.write_offset = 0;
                        }
                        while let Some(chunk) = sub.queue.pop_front() {
                            let pending = a.write_buf.len().saturating_sub(a.write_offset);
                            if pending.saturating_add(chunk.len()) > MAX_CLIENT_WRITE_BUF_BYTES {
                                sub.closed = true;
                                a.dead = true;
                                break;
                            }
                            a.write_buf.extend_from_slice(&chunk);
                        }
                    }
                } else {
                    a.dead = true;
                }
            }
        }

        let now = Instant::now();
        for a in &mut active {
            if a.dead {
                dead_ids.push(a.id);
                continue;
            }
            if a.write_offset >= a.write_buf.len()
                && now.saturating_duration_since(a.last_activity) >= keepalive_interval
            {
                a.write_buf.clear();
                a.write_offset = 0;
                a.write_buf.extend_from_slice(&ping_chunk);
            }
            flush_active_stream(a);
            if a.dead {
                dead_ids.push(a.id);
            }
        }

        if !dead_ids.is_empty() {
            active.retain(|a| !a.dead);
            let mut st = state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            for id in dead_ids {
                st.subscribers.remove(&id);
            }
        }

        let poll_wait = if active.is_empty() {
            Duration::from_millis(250)
        } else {
            let mut min_wait = Duration::from_millis(250);
            let now_for_wait = Instant::now();
            for a in &active {
                if a.write_offset < a.write_buf.len() {
                    min_wait = Duration::from_millis(10);
                    break;
                }
                let elapsed = now_for_wait.saturating_duration_since(a.last_activity);
                let rem = keepalive_interval.saturating_sub(elapsed);
                if rem < min_wait {
                    min_wait = rem;
                }
            }
            min_wait.clamp(Duration::from_millis(10), Duration::from_millis(250))
        };

        let mut pfds: Vec<PollFd<'_>> = Vec::with_capacity(1 + active.len());
        pfds.push(PollFd::from_borrowed_fd(wake_read.as_fd(), PollFlags::IN));
        for a in &active {
            let mut flags = PollFlags::IN;
            if a.write_offset < a.write_buf.len() {
                flags |= PollFlags::OUT;
            }
            pfds.push(PollFd::from_borrowed_fd(a.stream.as_fd(), flags));
        }

        let wait_us = poll_wait.as_micros() as u64;
        let ts = Timespec {
            tv_sec: (wait_us / 1_000_000) as i64,
            tv_nsec: ((wait_us % 1_000_000) * 1_000) as i64,
        };
        if let Err(err) = poll(&mut pfds, Some(&ts))
            && err != Errno::INTR
        {
            std::thread::sleep(Duration::from_millis(20));
            continue;
        }

        let wake_ready = pfds[0].revents().contains(PollFlags::IN);
        let mut client_revents: Vec<(usize, PollFlags)> = Vec::new();
        for (idx, pfd) in pfds.iter().enumerate().skip(1) {
            let rev = pfd.revents();
            if !rev.is_empty() {
                client_revents.push((idx - 1, rev));
            }
        }
        drop(pfds);

        if wake_ready {
            drain_wake_pipe(&wake_read);
        }

        let mut newly_dead: Vec<u64> = Vec::new();
        let mut scratch = [0u8; 64];
        for (idx, rev) in client_revents {
            if let Some(a) = active.get_mut(idx) {
                if rev.intersects(PollFlags::ERR | PollFlags::HUP | PollFlags::NVAL) {
                    a.dead = true;
                    newly_dead.push(a.id);
                    continue;
                }
                if rev.contains(PollFlags::IN) {
                    match a.stream.read(&mut scratch) {
                        Ok(0) => {
                            a.dead = true;
                            newly_dead.push(a.id);
                        }
                        Ok(_) => {}
                        Err(e)
                            if matches!(
                                e.kind(),
                                std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
                            ) => {}
                        Err(_) => {
                            a.dead = true;
                            newly_dead.push(a.id);
                        }
                    }
                }
            }
        }

        if !newly_dead.is_empty() {
            active.retain(|a| !a.dead);
            let mut st = state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            for id in newly_dead {
                st.subscribers.remove(&id);
            }
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
enum JsonValue {
    Null,
    Bool(bool),
    Number,
    String(String),
    Array(Vec<JsonValue>),
    Object(Vec<(String, JsonValue)>),
}

fn find_json_field<'a>(entries: &'a [(String, JsonValue)], key: &str) -> Option<&'a JsonValue> {
    entries.iter().rev().find(|(k, _)| k == key).map(|(_, v)| v)
}

fn parse_json(raw: &str) -> Option<JsonValue> {
    let mut parser = JsonParser {
        bytes: raw.as_bytes(),
        pos: 0,
    };
    let val = parser.parse_value(0)?;
    parser.skip_ws();
    if parser.pos == parser.bytes.len() {
        Some(val)
    } else {
        None
    }
}

struct JsonParser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl JsonParser<'_> {
    fn skip_ws(&mut self) {
        while self.pos < self.bytes.len()
            && matches!(self.bytes[self.pos], b' ' | b'\t' | b'\r' | b'\n')
        {
            self.pos += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn consume(&mut self, expected: &[u8]) -> bool {
        if self.bytes[self.pos..].starts_with(expected) {
            self.pos += expected.len();
            true
        } else {
            false
        }
    }

    fn parse_value(&mut self, depth: usize) -> Option<JsonValue> {
        if depth >= MAX_JSON_DEPTH {
            return None;
        }
        self.skip_ws();
        match self.peek()? {
            b'{' => self.parse_object(depth + 1),
            b'[' => self.parse_array(depth + 1),
            b'"' => self.parse_string().map(JsonValue::String),
            b't' => self.consume(b"true").then_some(JsonValue::Bool(true)),
            b'f' => self.consume(b"false").then_some(JsonValue::Bool(false)),
            b'n' => self.consume(b"null").then_some(JsonValue::Null),
            b'-' | b'0'..=b'9' => self.skip_number(),
            _ => None,
        }
    }

    fn skip_number(&mut self) -> Option<JsonValue> {
        if self.peek() == Some(b'-') {
            self.pos += 1;
        }
        let start = self.pos;
        while matches!(self.peek(), Some(b'0'..=b'9')) {
            self.pos += 1;
        }
        if self.pos == start {
            return None;
        }
        if self.peek() == Some(b'.') {
            self.pos += 1;
            let fstart = self.pos;
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
            if self.pos == fstart {
                return None;
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.pos += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.pos += 1;
            }
            let estart = self.pos;
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
            if self.pos == estart {
                return None;
            }
        }
        Some(JsonValue::Number)
    }

    fn parse_hex4(&mut self) -> Option<u16> {
        let hex = self.bytes.get(self.pos..self.pos + 4)?;
        let s = std::str::from_utf8(hex).ok()?;
        let v = u16::from_str_radix(s, 16).ok()?;
        self.pos += 4;
        Some(v)
    }

    fn parse_string(&mut self) -> Option<String> {
        if self.peek()? != b'"' {
            return None;
        }
        self.pos += 1;
        let mut out = Vec::new();
        while self.pos < self.bytes.len() {
            let b = self.bytes[self.pos];
            self.pos += 1;
            if b == b'"' {
                return String::from_utf8(out).ok();
            }
            if b < 0x20 {
                return None;
            }
            if b == b'\\' {
                let esc = *self.bytes.get(self.pos)?;
                self.pos += 1;
                match esc {
                    b'"' => out.push(b'"'),
                    b'\\' => out.push(b'\\'),
                    b'/' => out.push(b'/'),
                    b'b' => out.push(0x08),
                    b'f' => out.push(0x0c),
                    b'n' => out.push(b'\n'),
                    b'r' => out.push(b'\r'),
                    b't' => out.push(b'\t'),
                    b'u' => {
                        let code = self.parse_hex4()?;
                        let ch = if (0xD800..=0xDBFF).contains(&code) {
                            if !self.consume(b"\\u") {
                                return None;
                            }
                            let low = self.parse_hex4()?;
                            if !(0xDC00..=0xDFFF).contains(&low) {
                                return None;
                            }
                            let cp = 0x10000
                                + ((u32::from(code) - 0xD800) << 10)
                                + (u32::from(low) - 0xDC00);
                            char::from_u32(cp)?
                        } else if (0xDC00..=0xDFFF).contains(&code) {
                            return None;
                        } else {
                            char::from_u32(u32::from(code))?
                        };
                        let mut tmp = [0u8; 4];
                        out.extend_from_slice(ch.encode_utf8(&mut tmp).as_bytes());
                    }
                    _ => return None,
                }
            } else {
                out.push(b);
            }
        }
        None
    }

    fn parse_array(&mut self, depth: usize) -> Option<JsonValue> {
        self.pos += 1; // '['
        self.skip_ws();
        let mut items = Vec::new();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Some(JsonValue::Array(items));
        }
        loop {
            items.push(self.parse_value(depth)?);
            self.skip_ws();
            match self.peek()? {
                b',' => {
                    self.pos += 1;
                }
                b']' => {
                    self.pos += 1;
                    return Some(JsonValue::Array(items));
                }
                _ => return None,
            }
        }
    }

    fn parse_object(&mut self, depth: usize) -> Option<JsonValue> {
        self.pos += 1; // '{'
        self.skip_ws();
        let mut entries = Vec::new();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Some(JsonValue::Object(entries));
        }
        loop {
            self.skip_ws();
            let k = self.parse_string()?;
            self.skip_ws();
            if self.peek()? != b':' {
                return None;
            }
            self.pos += 1;
            let v = self.parse_value(depth)?;
            entries.push((k, v));
            self.skip_ws();
            match self.peek()? {
                b',' => {
                    self.pos += 1;
                }
                b'}' => {
                    self.pos += 1;
                    return Some(JsonValue::Object(entries));
                }
                _ => return None,
            }
        }
    }
}

#[cfg(all(unix, test))]
mod tests {
    use super::*;
    use std::net::TcpListener;

    #[test]
    fn directive_header_parsers_validate_and_sanitize() {
        let sub = parse_subscribe_header(
            r#"{"topics":[" page:1\r\n ", "claims", "page:1", ""], "last_event_id":" 42\n "}"#,
        );
        assert_eq!(
            sub,
            Some(SseSubscribeSpec {
                topics: vec!["page:1".to_string(), "claims".to_string()],
                last_event_id: Some("42".to_string()),
            })
        );
        assert_eq!(parse_subscribe_header(r#"{"topics":[]}"#), None);
        assert_eq!(parse_subscribe_header("not json"), None);

        let pub_ev = parse_publish_header(
            "{\"topic\":\" page:1\\n \",\"event\":\" blocks\\r \",\"data\":\"line1\\nline2\\u0021\\uD83D\\uDE00\"}",
        );
        assert_eq!(
            pub_ev,
            Some(SsePublishSpec {
                topic: "page:1".to_string(),
                event: Some("blocks".to_string()),
                data: "line1\nline2!\u{1F600}".to_string(),
            })
        );
        assert_eq!(
            parse_publish_header(r#"{"topic":"","event":null,"data":"x"}"#),
            None
        );
    }

    #[test]
    fn catchup_replays_newer_events_and_flags_eviction_or_cursor_ahead() {
        let mut st = BrokerState::new(8, 2, 16);
        st.next_seq = 1;
        // Simulate 3 events on "claims" with history_capacity=2 => seq 1 evicted.
        st.histories.insert(
            "claims".to_string(),
            TopicHistory {
                evicted_through: 1,
                events: VecDeque::from([
                    BrokerEvent {
                        seq: 2,
                        event: Some("claim.acquired".to_string()),
                        data: "{\"seq\":2}".to_string(),
                    },
                    BrokerEvent {
                        seq: 4,
                        event: Some("claim.released".to_string()),
                        data: "{\"seq\":4}".to_string(),
                    },
                ]),
                last_touched: 4,
            },
        );
        st.histories.insert(
            "page:a".to_string(),
            TopicHistory {
                evicted_through: 0,
                events: VecDeque::from([BrokerEvent {
                    seq: 3,
                    event: Some("page.updated".to_string()),
                    data: "{\"seq\":3}".to_string(),
                }]),
                last_touched: 3,
            },
        );
        st.next_seq = 5;

        // Cursor within retained history across multiple topics preserves global seq order.
        let clean_replay =
            st.collect_catchup_chunks(&["claims".into(), "page:a".into()], Some("2"));
        let joined = String::from_utf8(clean_replay.concat()).unwrap_or_default();
        assert!(joined.contains("id: 3\nevent: page.updated\ndata: {\"seq\":3}\n\n"));
        assert!(joined.contains("id: 4\nevent: claim.released\ndata: {\"seq\":4}\n\n"));
        assert!(!joined.contains("resync_required"));

        // Cursor equal to evicted_through means the evicted event was already seen: no gap!
        let boundary_replay = st.collect_catchup_chunks(&["claims".into()], Some("1"));
        let boundary_joined = String::from_utf8(boundary_replay.concat()).unwrap_or_default();
        assert!(!boundary_joined.contains("resync_required"));
        assert!(boundary_joined.contains("id: 2"));
        assert!(boundary_joined.contains("id: 4"));

        // Cursor strictly below evicted_through missed events -> emits resync_required before retained chunks.
        let evicted_replay = st.collect_catchup_chunks(&["claims".into()], Some("0"));
        let evicted_joined = String::from_utf8(evicted_replay.concat()).unwrap_or_default();
        assert!(evicted_joined.contains("event: resync_required"));
        assert!(evicted_joined.contains("history_evicted"));
        assert!(evicted_joined.contains("id: 2"));

        // Cursor ahead of next_seq (server restart) emits resync_required.
        let ahead_replay = st.collect_catchup_chunks(&["claims".into()], Some("99"));
        let ahead_joined = String::from_utf8(ahead_replay.concat()).unwrap_or_default();
        assert!(ahead_joined.contains("event: resync_required"));
        assert!(ahead_joined.contains("cursor_ahead"));
    }

    #[test]
    fn live_broker_streams_events_keepalives_disconnect_cleanup_and_capacity()
    -> Result<(), Box<dyn std::error::Error>> {
        let broker = SseBroker::with_limits(Duration::from_millis(80), 8, 16, 2)?;
        let listener = TcpListener::bind("127.0.0.1:0")?;
        let addr = listener.local_addr()?;

        let mut c1 = TcpStream::connect(addr)?;
        let (s1, _) = listener.accept()?;
        c1.set_read_timeout(Some(Duration::from_secs(2)))?;

        broker
            .register_client(
                s1,
                b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n"
                    .to_vec(),
                SseSubscribeSpec {
                    topics: vec!["page:alpha".to_string()],
                    last_event_id: None,
                },
            )
            .map_err(|_| "unexpected capacity rejection for c1")?;

        // Publish on page:alpha and verify prompt chunk delivery.
        broker.publish(&[SsePublishSpec {
            topic: "page:alpha".to_string(),
            event: Some("page.updated".to_string()),
            data: "{\"v\":1}".to_string(),
        }]);

        let mut received = Vec::new();
        let mut buf = [0u8; 1024];
        let deadline = Instant::now() + Duration::from_secs(2);
        while Instant::now() < deadline {
            let n = c1.read(&mut buf)?;
            if n == 0 {
                break;
            }
            received.extend_from_slice(&buf[..n]);
            let txt = String::from_utf8_lossy(&received);
            if txt.contains("page.updated") && txt.contains(": ping\n\n") {
                break;
            }
        }
        let txt = String::from_utf8_lossy(&received);
        assert!(
            txt.contains("id: 1\nevent: page.updated\ndata: {\"v\":1}\n\n"),
            "{txt}"
        );
        assert!(txt.contains(": ping\n\n"), "expected keepalive in {txt}");

        // Capacity cap (client_capacity = 2): second succeeds, third is rejected.
        let _c2 = TcpStream::connect(addr)?;
        let (s2, _) = listener.accept()?;
        broker
            .register_client(
                s2,
                Vec::new(),
                SseSubscribeSpec {
                    topics: vec!["page:beta".to_string()],
                    last_event_id: None,
                },
            )
            .map_err(|_| "unexpected capacity rejection for c2")?;

        let _c3 = TcpStream::connect(addr)?;
        let (s3, _) = listener.accept()?;
        let cap_res = broker.register_client(
            s3,
            Vec::new(),
            SseSubscribeSpec {
                topics: vec!["page:gamma".to_string()],
                last_event_id: None,
            },
        );
        assert!(cap_res.is_err(), "third client should exceed capacity=2");

        // Closing c1 is detected cleanly and decrements subscriber count.
        drop(c1);
        let cleanup_deadline = Instant::now() + Duration::from_secs(2);
        while broker.active_client_count() > 1 && Instant::now() < cleanup_deadline {
            std::thread::sleep(Duration::from_millis(20));
        }
        assert_eq!(broker.active_client_count(), 1);

        broker.shutdown();
        Ok(())
    }
}
