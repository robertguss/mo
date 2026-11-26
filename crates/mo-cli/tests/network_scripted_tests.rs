#![forbid(unsafe_code)]

use std::error::Error;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

use mo_def::limits::MO_LIMIT_RECEIVE_BYTES;

fn mo_bin() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_mo"))
}

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(label: &str) -> Result<Self, Box<dyn Error>> {
        static COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let id = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "mo-net-scripted-{}-{}-{}",
            std::process::id(),
            label,
            id
        ));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path)?;
        Ok(Self { path })
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

#[test]
fn network_fake_scripted_retries_truncation_allow_hosts_and_recorded_calls()
-> Result<(), Box<dyn Error>> {
    let dir = TempDir::new("positive")?;
    let main_mo = dir.path.join("main.mo");
    fs::write(
        &main_mo,
        r##"module Main
use Http

fn call_with_retry(net: Network, url: Text) : Result(Http.Response, Http.HttpError)
  var attempt = 0
  while attempt < 3
    case Http.post(url).header("X-Attempt", "#{attempt}").text("payload-#{attempt}").send(net)
      Ok(res):
        if res.ok?
          return Ok(res)
        end
      Error(_):
        Unit
    end
    attempt += 1
  end
  Http.post(url).header("X-Attempt", "final").text("payload-final").send(net)
end

test "ordered faults, truncation, allow_hosts narrowing, and recorded_calls"
  net = Network.fake_scripted([
    (
      "POST",
      "https://payments.example.org/v1/charge",
      [
        Network.Fail(error: Network.TimedOut),
        Network.Respond(response: Http.reply(503, "busy")),
        Network.Respond(response: Http.reply(200, "{\"charged\":true}"))
      ]
    ),
    (
      "GET",
      "https://payments.example.org/v1/stream",
      [
        Network.TruncateAfter(bytes: 0),
        Network.TruncateAfter(bytes: 12),
        Network.Respond(response: Http.reply(200, "complete-body"))
      ]
    ),
    (
      "GET",
      "https://api.example.org",
      [Network.Respond(response: Http.reply(200, "exact-host"))]
    ),
    (
      "GET",
      "https://blocked.example.org/secret",
      [Network.Respond(response: Http.reply(200, "should-never-run"))]
    )
  ])

  # Domain prefix must NOT match a lookalike suffix like api.example.org.evil.test
  lookalike = Http.get("https://api.example.org.evil.test/v1").send(net)
  assert lookalike == Error(Http.Unreachable(Network.Refused(host: "api.example.org.evil.test", port: 443)))

  # Spoofed Host header on raw Connection must NOT override connected host recorded in recorded_calls()
  raw_conn = case net.connect_secure("api.example.org", 443, 5.seconds)
    Ok(c): c
    Error(_): stop "connect"
  end
  assert raw_conn.send("GET /check HTTP/1.1\r\nHost: spoofed.evil.test\r\n\r\n".bytes) == Ok(Unit)
  assert raw_conn.receive(4096) is Ok(raw_reply)
  assert raw_reply.decode_utf8 is Some(raw_text) and raw_text.contains?("exact-host")
  recorded_so_far = net.recorded_calls()
  assert recorded_so_far.any?(fn(c) c.url == "https://api.example.org/check" end)
  assert not recorded_so_far.any?(fn(c) c.url.contains?("spoofed.evil.test") end)

  scoped = net.allow_hosts(["payments.example.org"])

  blocked = Http.get("https://blocked.example.org/secret").send(scoped)
  assert blocked == Error(Http.Unreachable(Network.Refused(host: "blocked.example.org", port: 443)))

  ok_res = call_with_retry(scoped, "https://payments.example.org/v1/charge")
  assert ok_res == Ok(Http.reply(200, "{\"charged\":true}"))

  cut0 = Http.get("https://payments.example.org/v1/stream").send(scoped)
  assert cut0 == Error(Http.BadResponse(reason: "the connection closed before the response was whole"))

  cut12 = Http.get("https://payments.example.org/v1/stream").send(scoped)
  assert cut12 == Error(Http.BadResponse(reason: "the connection closed before the response was whole"))

  whole = Http.get("https://payments.example.org/v1/stream").send(scoped)
  assert whole == Ok(Http.reply(200, "complete-body"))

  calls = Network.recorded_calls(net)
  assert calls.size == 8
  assert scoped.recorded_calls() == calls

  c0 = calls.get(2) or stop "c0"
  c1 = calls.get(3) or stop "c1"
  c2 = calls.get(4) or stop "c2"
  assert c0.method == "POST"
  assert c0.url == "https://payments.example.org/v1/charge"
  assert c0.body == "payload-0".bytes
  assert c0.headers.any?(fn(h) h == ("X-Attempt", "0") end)
  assert c1.body == "payload-1".bytes
  assert c2.body == "payload-2".bytes

  static_net = Network.fake(
    Http.replies([(Http.get("http://legacy.example.org/ping"), Http.reply(200, "pong"))])
  )
  assert Http.get("http://legacy.example.org/ping").send(static_net) == Ok(Http.reply(200, "pong"))
  assert Network.recorded_calls(static_net).size == 1

  ipv6_net = Network.fake_scripted([
    ("GET", "http://[::1]:8080/status", [Network.Respond(response: Http.reply(200, "ipv6"))])
  ])
  ipv6_conn = case ipv6_net.connect("::1", 8080, 5.seconds)
    Ok(c): c
    Error(_): stop "ipv6 connect"
  end
  assert ipv6_conn.send("GET /status HTTP/1.1\r\n\r\n".bytes) == Ok(Unit)
  assert ipv6_conn.receive(4096) is Ok(ipv6_reply)
  assert ipv6_reply.decode_utf8 is Some(ipv6_text) and ipv6_text.contains?("ipv6")
  assert ipv6_net.recorded_calls().get(0) is Some(call) and call.url == "http://[::1]:8080/status"
end
"##,
    )?;

    let out = Command::new(mo_bin())
        .args(["test", main_mo.to_str().unwrap_or("")])
        .output()?;
    assert!(
        out.status.success(),
        "expected mo test success:\nstdout: {}\nstderr: {}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    Ok(())
}

#[test]
fn network_scripted_assigns_replies_on_send_order() -> Result<(), Box<dyn Error>> {
    let dir = TempDir::new("send-order")?;
    let main_mo = dir.path.join("main.mo");
    fs::write(
        &main_mo,
        r#"module Main
use Http

test "assigns replies on send order"
  net = Network.fake_scripted([
    ("*", "/", [
      Network.Respond(response: Http.reply(200, "first")),
      Network.Respond(response: Http.reply(200, "second")),
      Network.Respond(response: Http.reply(200, "third"))
    ])
  ])
  assert net.connect("example.org", 80, 5.seconds) is Ok(c1)
  assert net.connect("example.org", 80, 5.seconds) is Ok(c2)
  assert net.connect("example.org", 80, 5.seconds) is Ok(c3)
  assert c1.send("POST /length HTTP/1.1\r\nContent-Length: 4\r\n\r\nab".bytes) == Ok(Unit)
  assert c1.receive(4096) == Error(Network.TimedOut)
  assert c2.send("POST /chunk HTTP/1.1\r\nTransfer-Encoding: Chunked\r\n\r\n5\r\n0\r\n\r\n\r\n".bytes) == Ok(Unit)
  assert net.recorded_calls().size == 0
  assert c2.send("0\r\nTrailer: done\r\n".bytes) == Ok(Unit)
  assert net.recorded_calls().size == 0
  assert c2.send("\r\n".bytes) == Ok(Unit)
  assert c1.send("cd".bytes) == Ok(Unit)
  assert c3.send("GET /unread HTTP/1.1\r\n\r\n".bytes) == Ok(Unit)
  calls = net.recorded_calls()
  assert calls.size == 3
  assert calls.get(0) is Some(chunked) and chunked.url == "http://example.org/chunk"
  assert calls.get(1) is Some(length) and length.url == "http://example.org/length"
  assert length.body == "abcd".bytes
  assert calls.get(2) is Some(unread) and unread.url == "http://example.org/unread"
  assert c1.receive(4096) is Ok(second) and second.decode_utf8 is Some(second_text)
  assert c2.receive(4096) is Ok(first) and first.decode_utf8 is Some(first_text)
  assert second_text.contains?("second")
  assert first_text.contains?("first")
end
"#,
    )?;
    let out = Command::new(mo_bin())
        .args(["test", main_mo.to_str().unwrap_or("")])
        .output()?;
    assert!(
        out.status.success(),
        "expected mo test success:\nstdout: {}\nstderr: {}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    Ok(())
}

#[test]
fn network_scripted_truncation_can_span_multiple_reads() -> Result<(), Box<dyn Error>> {
    let dir = TempDir::new("multi-read-truncation")?;
    let main = dir.path.join("main.mo");
    let read_limit = MO_LIMIT_RECEIVE_BYTES;
    let prefix_len = read_limit + 64;
    let declared_len = prefix_len + 64;
    let header_len = format!("HTTP/1.1 200 OK\r\nContent-Length: {declared_len}\r\n\r\n").len();
    fs::write(
        &main,
        format!(
            r#"module Main

test "truncation counts payload bytes across reads"
  net = Network.fake_scripted([("GET", "/", [Network.TruncateAfter(bytes: {prefix_len})])])
  assert net.connect("example.org", 80, 5.seconds) is Ok(conn)
  assert conn.send("GET / HTTP/1.1\r\n\r\n".bytes) == Ok(Unit)
  assert conn.receive({read_limit}) is Ok(first)
  assert first.size == {read_limit}
  assert conn.receive({read_limit}) is Ok(second)
  assert first.size + second.size == {prefix_len} + {header_len}
  assert conn.receive({read_limit}) == Ok("".bytes)
end
"#,
        ),
    )?;
    let out = Command::new(mo_bin()).arg("test").arg(main).output()?;
    assert!(
        out.status.success(),
        "stdout: {}\nstderr: {}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    Ok(())
}

#[test]
fn network_fake_scripted_and_recorded_calls_rejected_outside_spec() -> Result<(), Box<dyn Error>> {
    let cases: [(&str, &str, &str); 2] = [
        (
            "fake_scripted_outside_spec",
            r#"module Main
use Http

fn make_net() : Network
  Network.fake_scripted([("GET", "https://example.org", [Network.Respond(response: Http.reply(200, "ok"))])])
end

fn main()
  _ = make_net()
end
"#,
            "E0098",
        ),
        (
            "recorded_calls_outside_spec",
            r#"module Main

fn spy(net: Network) : Int
  Network.recorded_calls(net).size
end

fn main(net: Network)
  _ = spy(net)
end
"#,
            "E0098",
        ),
    ];

    for (label, src, expected_code) in cases {
        let dir = TempDir::new(label)?;
        let main_mo = dir.path.join("main.mo");
        fs::write(&main_mo, src)?;
        let out = Command::new(mo_bin())
            .args(["check", main_mo.to_str().unwrap_or("")])
            .output()?;
        assert!(!out.status.success(), "case {label} unexpectedly succeeded");
        let combined = format!(
            "{}{}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        assert!(
            combined.contains(expected_code),
            "case {label} expected {expected_code}, got:\n{combined}"
        );
    }
    Ok(())
}
