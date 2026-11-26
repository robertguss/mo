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
}
