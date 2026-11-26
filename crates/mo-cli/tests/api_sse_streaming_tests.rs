#![forbid(unsafe_code)]

//! Live multi-worker HTTP/1.1 streaming SSE tests (`mo serve` + `SseBroker` + `Api.sse_*`).

#[cfg(all(unix, test))]
mod unix_tests {
    use std::error::Error;
    use std::fs;
    use std::io::{BufRead, BufReader, Read, Write};
    use std::net::TcpStream;
    use std::path::{Path, PathBuf};
    use std::process::{Child, Command, Stdio};
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{Duration, Instant};

    static SEQ: AtomicU64 = AtomicU64::new(1);

    fn mo_bin() -> PathBuf {
        PathBuf::from(env!("CARGO_BIN_EXE_mo"))
    }

    fn temp_dir(tag: &str) -> Result<PathBuf, Box<dyn Error>> {
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!("mo-api-sse-{tag}-{}-{n}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir)?;
        Ok(dir)
    }

    struct ServeGuard {
        child: Child,
    }

    impl Drop for ServeGuard {
        fn drop(&mut self) {
            if let Ok(None) = self.child.try_wait() {
                let _ = self.child.kill();
            }
            let _ = self.child.wait();
        }
    }

    fn wait_ready_port(ready_path: &Path, child: &mut Child) -> Result<u16, Box<dyn Error>> {
        let deadline = Instant::now() + Duration::from_secs(15);
        while Instant::now() < deadline {
            if let Ok(raw) = fs::read_to_string(ready_path)
                && let Some(idx) = raw.find("\"port\":")
            {
                let rest = &raw[idx + 7..];
                let digits: String = rest.chars().take_while(char::is_ascii_digit).collect();
                if let Ok(port) = digits.parse::<u16>() {
                    return Ok(port);
                }
            }
            if let Some(status) = child.try_wait()? {
                let mut err_buf = String::new();
                if let Some(mut stderr) = child.stderr.take() {
                    let _ = stderr.read_to_string(&mut err_buf);
                }
                return Err(format!("mo serve exited early with {status}: {err_buf}").into());
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        Err("timed out waiting for mo serve ready file".into())
    }

    struct SseStreamReader {
        headers: String,
        reader: BufReader<TcpStream>,
        leftover: String,
    }

    fn open_sse_stream(
        port: u16,
        path: &str,
        last_event_id: Option<&str>,
    ) -> Result<SseStreamReader, Box<dyn Error>> {
        let mut stream = TcpStream::connect(("127.0.0.1", port))?;
        stream.set_read_timeout(Some(Duration::from_secs(5)))?;
        let mut req = format!(
            "GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nAccept: text/event-stream\r\n"
        );
        if let Some(id) = last_event_id {
            req.push_str(&format!("Last-Event-ID: {id}\r\n"));
        }
        req.push_str("\r\n");
        stream.write_all(req.as_bytes())?;
        stream.flush()?;

        let mut reader = BufReader::new(stream);
        let mut headers = String::new();
        loop {
            let mut line = String::new();
            let n = reader.read_line(&mut line)?;
            if n == 0 {
                return Err("unexpected EOF while reading SSE headers".into());
            }
            headers.push_str(&line);
            if headers.ends_with("\r\n\r\n") {
                break;
            }
        }
        Ok(SseStreamReader {
            headers,
            reader,
            leftover: String::new(),
        })
    }

    impl SseStreamReader {
        fn read_next_chunk_text(&mut self) -> Result<String, Box<dyn Error>> {
            let mut size_line = String::new();
            let n = self.reader.read_line(&mut size_line)?;
            if n == 0 {
                return Err("EOF reading HTTP chunk size".into());
            }
            let trimmed = size_line.trim();
            let hex_only = trimmed.split(';').next().unwrap_or("");
            let len = usize::from_str_radix(hex_only, 16)?;
            if len == 0 {
                return Ok(String::new());
            }
            let mut body = vec![0u8; len];
            self.reader.read_exact(&mut body)?;
            let mut crlf = [0u8; 2];
            self.reader.read_exact(&mut crlf)?;
            Ok(String::from_utf8(body)?)
        }

        fn read_until_contains(&mut self, needle: &str) -> Result<String, Box<dyn Error>> {
            let deadline = Instant::now() + Duration::from_secs(5);
            while !self.leftover.contains(needle) {
                if Instant::now() >= deadline {
                    return Err(format!(
                        "timed out waiting for '{needle}' in SSE stream: {}",
                        self.leftover
                    )
                    .into());
                }
                let chunk = self.read_next_chunk_text()?;
                if chunk.is_empty() {
                    return Err("stream ended before expected SSE frame".into());
                }
                self.leftover.push_str(&chunk);
            }
            Ok(self.leftover.clone())
        }
    }

    fn http_request(
        port: u16,
        method: &str,
        path: &str,
        body: Option<&str>,
    ) -> Result<(u16, String, String), Box<dyn Error>> {
        let mut stream = TcpStream::connect(("127.0.0.1", port))?;
        stream.set_read_timeout(Some(Duration::from_secs(5)))?;
        let payload = body.unwrap_or("");
        let mut req =
            format!("{method} {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n");
        if body.is_some() {
            req.push_str(&format!(
                "Content-Type: application/json\r\nContent-Length: {}\r\n",
                payload.len()
            ));
        }
        req.push_str("\r\n");
        if body.is_some() {
            req.push_str(payload);
        }
        stream.write_all(req.as_bytes())?;
        stream.flush()?;

        let mut raw = String::new();
        stream.read_to_string(&mut raw)?;
        let Some((hdrs, bdy)) = raw.split_once("\r\n\r\n") else {
            return Err(format!("malformed HTTP response: {raw}").into());
        };
        let status = hdrs
            .lines()
            .next()
            .and_then(|l| l.split_whitespace().nth(1))
            .and_then(|c| c.parse::<u16>().ok())
            .ok_or("missing status code")?;
        Ok((status, hdrs.to_string(), bdy.to_string()))
    }

    #[test]
    fn mo_serve_streams_sse_across_workers_without_starvation() -> Result<(), Box<dyn Error>> {
        let dir = temp_dir("multiworker")?;
        let main_mo = dir.join("main.mo");
        let ready_file = dir.join("ready.json");

        let src = r##"module Main
use Api
use Json

struct EmitBody
  topic: Text
  msg: Text
end

api
  port 0
  get "/ping" ping
  get "/events" events
  post "/emit" emit
end

fn ping() : Text
  "pong"
end

fn events(req: Api.Request) : Api.Response
  topic = req.query_param("topic") or "room:a"
  Api.sse_stream_with(
    [topic],
    req.header("Last-Event-ID"),
    Some(2000),
    [(Some("ready"), "{\"topic\":\"#{topic}\"}")]
  )
end

fn emit(input: EmitBody) : Api.Response
  Api.json_response(200, {"ok": true}).with_sse_publish_json(
    input.topic,
    "msg",
    Json.Object({"topic": Json.String(input.topic), "msg": Json.String(input.msg)})
  )
end

fn main() : Int
  0
end
"##;
        fs::write(&main_mo, src)?;

        let child = Command::new(mo_bin())
            .arg("serve")
            .arg(&main_mo)
            .arg("--bind")
            .arg("127.0.0.1")
            .arg("--port")
            .arg("0")
            .arg("--workers")
            .arg("4")
            .arg("--data")
            .arg(&dir)
            .arg("--ready-file")
            .arg(&ready_file)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?;
        let mut guard = ServeGuard { child };
        let port = wait_ready_port(&ready_file, &mut guard.child)?;

        // 1. Open SSE streams on room:a and room:b; verify chunked framing & no leaked internal headers.
        let mut sub_a = open_sse_stream(port, "/events?topic=room:a", None)?;
        assert!(
            sub_a.headers.starts_with("HTTP/1.1 200 OK"),
            "{}",
            sub_a.headers
        );
        let lower_hdrs_a = sub_a.headers.to_ascii_lowercase();
        assert!(lower_hdrs_a.contains("transfer-encoding: chunked"));
        assert!(!lower_hdrs_a.contains("x-mo-sse-subscribe"));
        assert!(!lower_hdrs_a.contains("x-mo-sse-publish"));
        sub_a.read_until_contains("event: ready\ndata: {\"topic\":\"room:a\"}\n\n")?;

        let mut sub_b = open_sse_stream(port, "/events?topic=room:b", None)?;
        sub_b.read_until_contains("event: ready\ndata: {\"topic\":\"room:b\"}\n\n")?;

        // 2. Open 4 more SSE subscribers (6 total > --workers 4) and prove HTTP requests never starve.
        let mut extras = Vec::new();
        for _ in 0..4 {
            let mut s = open_sse_stream(port, "/events?topic=room:extra", None)?;
            s.read_until_contains("event: ready")?;
            extras.push(s);
        }
        for _ in 0..8 {
            let (st, _, bdy) = http_request(port, "GET", "/ping", None)?;
            assert_eq!(st, 200);
            assert_eq!(bdy, "\"pong\"");
        }

        // 3. Emit to room:a -> delivered to sub_a with id: 1; verify directive stripped from /emit response.
        let (emit_st, emit_hdrs, emit_bdy) = http_request(
            port,
            "POST",
            "/emit",
            Some("{\"topic\":\"room:a\",\"msg\":\"hello\"}"),
        )?;
        assert_eq!(emit_st, 200, "{emit_bdy}");
        assert!(!emit_hdrs.to_ascii_lowercase().contains("x-mo-sse-"));
        let received_a = sub_a.read_until_contains("id: 1\nevent: msg\n")?;
        assert!(received_a.contains("\"msg\":\"hello\""));

        // Emit to room:b (id: 2) -> verify sub_b receives id: 2 and sub_a stays isolated (never sees room:b msg).
        let (emit_b_st, _, _) = http_request(
            port,
            "POST",
            "/emit",
            Some("{\"topic\":\"room:b\",\"msg\":\"beta-only\"}"),
        )?;
        assert_eq!(emit_b_st, 200);
        sub_b.read_until_contains("beta-only")?;

        // 4. Close sub_a, emit id: 3 while disconnected, reconnect with Last-Event-ID: 1 -> replays id: 3, not id: 2!
        drop(sub_a);
        let (emit_3_st, _, _) = http_request(
            port,
            "POST",
            "/emit",
            Some("{\"topic\":\"room:a\",\"msg\":\"missed-while-offline\"}"),
        )?;
        assert_eq!(emit_3_st, 200);

        let mut reconnected_a = open_sse_stream(port, "/events?topic=room:a", Some("1"))?;
        let replayed = reconnected_a.read_until_contains("missed-while-offline")?;
        assert!(replayed.contains("id: 3\nevent: msg"));
        assert!(!replayed.contains("beta-only"));
        assert!(!replayed.contains("resync_required"));

        // 5. Cursor ahead of server seq (simulating server restart) emits resync_required.
        let mut stale_cursor = open_sse_stream(port, "/events?topic=room:a", Some("9999"))?;
        stale_cursor.read_until_contains("event: resync_required")?;

        // 6. Clean SIGTERM shutdown while SSE streams remain open.
        if let Some(pid) = rustix::process::Pid::from_raw(guard.child.id() as i32) {
            rustix::process::kill_process(pid, rustix::process::Signal::TERM)?;
        }
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(status) = guard.child.try_wait()? {
                assert!(
                    status.success(),
                    "expected clean SIGTERM exit, got {status}"
                );
                break;
            }
            if Instant::now() >= deadline {
                return Err("mo serve did not exit cleanly after SIGTERM with SSE clients".into());
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        drop(extras);
        drop(reconnected_a);
        drop(stale_cursor);
        drop(sub_b);

        let _ = fs::remove_dir_all(&dir);
        Ok(())
    }

    #[test]
    fn mo_run_http_open_stream_reads_incremental_sse_from_live_loopback_peer()
    -> Result<(), Box<dyn Error>> {
        use std::net::TcpListener;

        let listener = TcpListener::bind("127.0.0.1:0")?;
        let peer_port = listener.local_addr()?.port();
        listener.set_nonblocking(true)?;
        let stop_peer = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let stop_peer_bg = std::sync::Arc::clone(&stop_peer);
        let peer_thread = std::thread::spawn(move || {
            while !stop_peer_bg.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((mut sock, _)) => {
                        let _ = sock.set_read_timeout(Some(Duration::from_secs(2)));
                        let mut req_buf = [0u8; 1024];
                        let _ = sock.read(&mut req_buf);
                        let _ = sock.write_all(
                            b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n",
                        );
                        let part1 =
                            b": keepalive\n\nid: 1\nevent: ai.token\ndata: {\"delta\":\"Hel";
                        let _ = sock.write_all(format!("{:x}\r\n", part1.len()).as_bytes());
                        let _ = sock.write_all(part1);
                        let _ = sock.write_all(b"\r\n");
                        let _ = sock.flush();
                        std::thread::sleep(Duration::from_millis(60));
                        let part2 = b"lo\"}\n\nevent: ai.done\ndata: {\"model\":\"m1\"}\n\n";
                        let _ = sock.write_all(format!("{:x}\r\n", part2.len()).as_bytes());
                        let _ = sock.write_all(part2);
                        let _ = sock.write_all(b"\r\n0\r\n\r\n");
                        let _ = sock.flush();
                        break;
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                        std::thread::sleep(Duration::from_millis(10));
                    }
                    Err(_) => break,
                }
            }
        });

        let dir = temp_dir("http-open-stream")?;
        let prog = dir.join("stream-demo.mo");
        let src = r###"module StreamDemo
use Http

fn main(args: Args, console: Console, network: Network) : Result(Unit, Http.HttpError)
  url = args.list.first or ""
  var st = try Http.get(url).open_stream(network)
  console.print("status=#{st.stream_status}")
  var done = false
  while not done
    (ev_opt, next_st) = try st.next_sse_event
    st = next_st
    case ev_opt
      Some(ev):
        name = ev.event or "message"
        console.print("#{name}|#{ev.data}")
      None: done = true
    end
  end
  Ok(Unit)
end
"###;
        fs::write(&prog, src)?;

        let out = Command::new(mo_bin())
            .arg("run")
            .arg(&prog)
            .arg(format!("http://127.0.0.1:{peer_port}/stream"))
            .output()?;
        stop_peer.store(true, Ordering::Relaxed);
        peer_thread.join().unwrap();
        let _ = fs::remove_dir_all(&dir);

        assert!(
            out.status.success(),
            "mo run failed: {}",
            String::from_utf8_lossy(&out.stderr)
        );
        let stdout = String::from_utf8(out.stdout)?;
        assert_eq!(
            stdout,
            "status=200\nai.token|{\"delta\":\"Hello\"}\nai.done|{\"model\":\"m1\"}\n"
        );
        Ok(())
    }

    fn open_post_sse_stream(
        port: u16,
        path: &str,
        body: &str,
    ) -> Result<SseStreamReader, Box<dyn Error>> {
        let mut stream = TcpStream::connect(("127.0.0.1", port))?;
        stream.set_read_timeout(Some(Duration::from_secs(5)))?;
        let req = format!(
            "POST {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nAccept: text/event-stream\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{body}",
            body.len()
        );
        stream.write_all(req.as_bytes())?;
        stream.flush()?;

        let mut reader = BufReader::new(stream);
        let mut headers = String::new();
        loop {
            let mut line = String::new();
            let n = reader.read_line(&mut line)?;
            if n == 0 {
                return Err("unexpected EOF while reading SSE relay headers".into());
            }
            headers.push_str(&line);
            if headers.ends_with("\r\n\r\n") {
                break;
            }
        }
        Ok(SseStreamReader {
            headers,
            reader,
            leftover: String::new(),
        })
    }

    #[test]
    fn mo_serve_streams_outbound_http_sse_relays_incrementally_without_starving_workers()
    -> Result<(), Box<dyn Error>> {
        use std::net::TcpListener;

        let upstream = TcpListener::bind("127.0.0.1:0")?;
        let up_port = upstream.local_addr()?.port();
        let stop_up = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let stop_up_bg = std::sync::Arc::clone(&stop_up);
        upstream.set_nonblocking(true)?;
        let up_thread = std::thread::spawn(move || {
            let mut workers = Vec::new();
            while !stop_up_bg.load(Ordering::Relaxed) {
                match upstream.accept() {
                    Ok((mut sock, _)) => {
                        workers.push(std::thread::spawn(move || {
                            let _ = sock.set_read_timeout(Some(Duration::from_secs(2)));
                            let mut buf = Vec::new();
                            let mut tmp = [0u8; 1024];
                            loop {
                                match sock.read(&mut tmp) {
                                    Ok(0) => break,
                                    Ok(n) => {
                                        buf.extend_from_slice(&tmp[..n]);
                                        if buf.windows(4).any(|w| w == b"\r\n\r\n") {
                                            break;
                                        }
                                    }
                                    Err(_) => return,
                                }
                            }
                            let req_str = String::from_utf8_lossy(&buf);
                            let path = req_str
                                .lines()
                                .next()
                                .and_then(|l| l.split_whitespace().nth(1))
                                .unwrap_or("/");
                            match path {
                                "/v1/chat/completions" => {
                                    if !req_str.to_ascii_lowercase().contains("authorization: bearer upstream-secret-xyz") {
                                        let _ = sock.write_all(b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
                                        return;
                                    }
                                    let _ = sock.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
                                    let _ = sock.flush();
                                    for tok in ["Rotate ", "gateway ", "keys."] {
                                        std::thread::sleep(Duration::from_millis(120));
                                        let sse = format!(
                                            "data: {{\"choices\":[{{\"delta\":{{\"content\":\"{tok}\"}}}}]}}\n\n"
                                        );
                                        let chunk = format!("{:x}\r\n{sse}\r\n", sse.len());
                                        let _ = sock.write_all(chunk.as_bytes());
                                        let _ = sock.flush();
                                    }
                                    let done = b"data: [DONE]\n\n";
                                    let _ = sock.write_all(
                                        format!("{:x}\r\n", done.len()).as_bytes(),
                                    );
                                    let _ = sock.write_all(done);
                                    let _ = sock.write_all(b"\r\n0\r\n\r\n");
                                    let _ = sock.flush();
                                }
                                "/v1/messages" => {
                                    let _ = sock.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
                                    for tok in ["Anthropic ", "stream."] {
                                        std::thread::sleep(Duration::from_millis(60));
                                        let sse = format!(
                                            "event: content_block_delta\ndata: {{\"type\":\"content_block_delta\",\"delta\":{{\"type\":\"text_delta\",\"text\":\"{tok}\"}}}}\n\n"
                                        );
                                        let _ = sock.write_all(format!("{:x}\r\n{sse}\r\n", sse.len()).as_bytes());
                                        let _ = sock.flush();
                                    }
                                    let stop_sse = b"event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
                                    let _ = sock.write_all(format!("{:x}\r\n", stop_sse.len()).as_bytes());
                                    let _ = sock.write_all(stop_sse);
                                    let _ = sock.write_all(b"\r\n0\r\n\r\n");
                                    let _ = sock.flush();
                                }
                                "/slow" => {
                                    let _ = sock.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
                                    let _ = sock.flush();
                                    std::thread::sleep(Duration::from_millis(600));
                                }
                                _ => {
                                    let _ = sock.write_all(b"HTTP/1.1 429 Too Many Requests\r\nAuthorization-Leak: upstream-secret-xyz\r\nContent-Length: 10\r\n\r\nrate limit");
                                    let _ = sock.flush();
                                }
                            }
                        }));
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                        std::thread::sleep(Duration::from_millis(10));
                    }
                    Err(_) => break,
                }
            }
            for h in workers {
                let _ = h.join();
            }
        });

        let dir = temp_dir("relay")?;
        let main_mo = dir.join("main.mo");
        let ready_file = dir.join("ready.json");
        let src = format!(
            r##"module Main
use Api

api
  port 0
  get "/ping" ping
  post "/ask-openai" ask_openai
  post "/ask-anthropic" ask_anthropic
  post "/ask-timeout" ask_timeout
  post "/ask-429" ask_429
end

fn ping() : Text
  "pong"
end

fn ask_openai(_req: Api.Request) : Api.Response
  relay = Api.SseRelay(
    method: "POST",
    url: "http://127.0.0.1:{up_port}/v1/chat/completions",
    headers: [("Authorization", "Bearer upstream-secret-xyz"), ("Content-Type", "application/json")],
    body: "{{\"stream\":true}}",
    mode: "openai_sse",
    idle_timeout_ms: 5000,
    token_event: Some("ai.token"),
    done_event: Some("ai.done"),
    done_data: Some("{{\"model\":\"gpt-4o\"}}"),
    error_event: Some("ai.error")
  )
  Api.sse_relay_stream([(Some("ai.citation"), "{{\"page\":\"p1\"}}")], relay)
end

fn ask_anthropic(_req: Api.Request) : Api.Response
  relay = Api.SseRelay(
    method: "POST",
    url: "http://127.0.0.1:{up_port}/v1/messages",
    headers: [("x-api-key", "ant-secret"), ("Content-Type", "application/json")],
    body: "{{\"stream\":true}}",
    mode: "anthropic_sse",
    idle_timeout_ms: 5000,
    token_event: Some("ai.token"),
    done_event: Some("ai.done"),
    done_data: Some("{{\"model\":\"claude\"}}"),
    error_event: Some("ai.error")
  )
  Api.sse_relay_stream([], relay)
end

fn ask_timeout(_req: Api.Request) : Api.Response
  relay = Api.SseRelay(
    method: "POST",
    url: "http://127.0.0.1:{up_port}/slow",
    headers: [("Authorization", "Bearer upstream-secret-xyz")],
    body: "{{}}",
    mode: "openai_sse",
    idle_timeout_ms: 250,
    token_event: Some("ai.token"),
    done_event: Some("ai.done"),
    done_data: None,
    error_event: Some("ai.error")
  )
  Api.sse_relay_stream([(Some("ai.citation"), "{{\"page\":\"p1\"}}")], relay)
end

fn ask_429(_req: Api.Request) : Api.Response
  relay = Api.SseRelay(
    method: "POST",
    url: "http://127.0.0.1:{up_port}/err429",
    headers: [("Authorization", "Bearer upstream-secret-xyz")],
    body: "{{}}",
    mode: "openai_sse",
    idle_timeout_ms: 5000,
    token_event: Some("ai.token"),
    done_event: Some("ai.done"),
    done_data: None,
    error_event: Some("ai.error")
  )
  Api.sse_relay_stream([], relay)
end

fn main() : Int
  0
end
"##
        );
        fs::write(&main_mo, src)?;

        let child = Command::new(mo_bin())
            .arg("serve")
            .arg(&main_mo)
            .arg("--bind")
            .arg("127.0.0.1")
            .arg("--port")
            .arg("0")
            .arg("--workers")
            .arg("2")
            .arg("--data")
            .arg(&dir)
            .arg("--ready-file")
            .arg(&ready_file)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?;
        let mut guard = ServeGuard { child };
        let port = wait_ready_port(&ready_file, &mut guard.child)?;

        // 1. Open 3 concurrent slow OpenAI relay streams (> --workers 2) while pinging /ping.
        let t_start = Instant::now();
        let mut r1 = open_post_sse_stream(port, "/ask-openai", "{}")?;
        let mut r2 = open_post_sse_stream(port, "/ask-openai", "{}")?;
        let mut r3 = open_post_sse_stream(port, "/ask-openai", "{}")?;
        let lower_h = r1.headers.to_ascii_lowercase();
        assert!(lower_h.starts_with("http/1.1 200 ok"));
        assert!(!lower_h.contains("x-mo-sse-relay"));
        assert!(!lower_h.contains("upstream-secret-xyz"));
        r1.read_until_contains("event: ai.citation\ndata: {\"page\":\"p1\"}\n\n")?;
        let t_cit = t_start.elapsed();
        assert!(
            t_cit < Duration::from_millis(250),
            "initial citation delayed: {t_cit:?}"
        );

        let (pst, _, pbdy) = http_request(port, "GET", "/ping", None)?;
        assert_eq!(pst, 200);
        assert_eq!(pbdy, "\"pong\"");

        let full1 = r1.read_until_contains("event: ai.done\ndata: {\"model\":\"gpt-4o\"}\n\n")?;
        let t_done = t_start.elapsed();
        assert!(
            t_done.saturating_sub(t_cit) >= Duration::from_millis(200),
            "tokens were buffered instead of streamed: t_cit={t_cit:?}, t_done={t_done:?}"
        );
        assert!(full1.contains("data: {\"delta\":\"Rotate \"}"));
        assert!(full1.contains("data: {\"delta\":\"gateway \"}"));
        assert!(full1.contains("data: {\"delta\":\"keys.\"}"));
        assert!(!full1.contains("upstream-secret-xyz"));
        assert_eq!(r1.read_next_chunk_text()?, "");

        r2.read_until_contains("event: ai.done")?;
        r3.read_until_contains("event: ai.done")?;

        // 2. Anthropic SSE transform
        let mut ant = open_post_sse_stream(port, "/ask-anthropic", "{}")?;
        let ant_body =
            ant.read_until_contains("event: ai.done\ndata: {\"model\":\"claude\"}\n\n")?;
        assert!(ant_body.contains("data: {\"delta\":\"Anthropic \"}"));
        assert!(ant_body.contains("data: {\"delta\":\"stream.\"}"));
        assert!(!ant_body.contains("ant-secret"));

        // 3. Idle timeout emits sanitized ai.error without ai.done
        let mut to_stream = open_post_sse_stream(port, "/ask-timeout", "{}")?;
        let to_body = to_stream.read_until_contains("event: ai.error")?;
        assert!(to_body.contains("\"upstream_timeout\""));
        assert!(!to_body.contains("event: ai.done"));
        assert!(!to_body.contains("upstream-secret-xyz"));
        assert_eq!(to_stream.read_next_chunk_text()?, "");

        // 4. Non-2xx upstream status emits upstream_http_status without leaking upstream headers/secrets
        let mut err_stream = open_post_sse_stream(port, "/ask-429", "{}")?;
        let err_body = err_stream.read_until_contains("event: ai.error")?;
        assert!(err_body.contains("\"upstream_http_status\""));
        assert!(err_body.contains("\"status\":429"));
        assert!(!err_body.contains("upstream-secret-xyz"));
        assert_eq!(err_stream.read_next_chunk_text()?, "");

        if let Some(pid) = rustix::process::Pid::from_raw(guard.child.id() as i32) {
            rustix::process::kill_process(pid, rustix::process::Signal::TERM)?;
        }
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(status) = guard.child.try_wait()? {
                assert!(
                    status.success(),
                    "expected clean SIGTERM exit, got {status}"
                );
                break;
            }
            if Instant::now() >= deadline {
                return Err("mo serve did not exit cleanly after SIGTERM".into());
            }
            std::thread::sleep(Duration::from_millis(20));
        }

        stop_up.store(true, Ordering::Relaxed);
        up_thread.join().unwrap();
        let _ = fs::remove_dir_all(&dir);
        Ok(())
    }
}
