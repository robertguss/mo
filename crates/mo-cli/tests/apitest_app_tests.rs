#![forbid(unsafe_code)]

//! End-to-end positive and negative ("able to fail") tests for `ApiTest.start`,
//! `ApiTest.App`, and `ApiTest.RouteSpec`.

use std::error::Error;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::atomic::{AtomicU64, Ordering};

type TestResult = Result<(), Box<dyn Error>>;

fn mo_bin() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_mo"))
}

fn unique_temp_dir(tag: &str) -> io::Result<PathBuf> {
    static SEQ: AtomicU64 = AtomicU64::new(1);
    let n = SEQ.fetch_add(1, Ordering::Relaxed);
    let dir = std::env::temp_dir().join(format!("mo-apitest-app-{tag}-{}-{n}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir)?;
    Ok(dir)
}

fn run_mo(args: &[&str], dir: &Path) -> io::Result<Output> {
    Command::new(mo_bin()).args(args).current_dir(dir).output()
}

#[test]
fn apitest_app_shares_fakes_across_copies_helpers_clock_sleep_and_multiple_apis() -> TestResult {
    let dir = unique_temp_dir("positive")?;
    fs::write(
        dir.join("other-api.mo"),
        r#"module OtherApi

api
  port 8081
  get "/ping" ping
end

fn ping() : Text
  "pong"
end
"#,
    )?;

    // Helper module that mentions ONLY `ApiTest.App` and `ApiTest.RouteSpec` without `use Api` or `use Json`.
    fs::write(
        dir.join("helpers.mo"),
        r##"module Helpers
expose route_paths, openapi_len

fn route_paths(app: ApiTest.App) : List(Text)
  var out = []
  for r in app.routes()
    out = out.append("#{r.method} #{r.path_template} (#{r.auth_mode})")
  end
  out
end

fn openapi_len(app: ApiTest.App) : Int
  app.openapi_json().byte_count
end
"##,
    )?;

    fs::write(
        dir.join("main.mo"),
        r##"module Main
use Api
use Helpers
use Json
use OtherApi

actor Counter
  state Int = 0
  calls bump, current
end

fn bump(s: Int, _now: Time) : (Int, Int)
  (s + 1, s + 1)
end

fn current(s: Int, _now: Time) : Text
  "#{s}"
end

struct ItemBody
  name: Text
end

api
  port 8080
  sqlite db from "app.db"
  actor counter: Counter
  job "expire.items" run_expire
  get "/now" read_now
  scope "/items"
    post "" create_item
    get "" list_items
  end
end

fn read_now(clock: Clock) : Text
  clock.now.to_text
end

fn ensure_table(db: Sqlite.Database)
  case db.execute(
    "CREATE TABLE IF NOT EXISTS items (name TEXT NOT NULL, created_at TEXT NOT NULL)",
    []
  )
    Ok(_): Unit
    Error(_): stop "sql failed"
  end
end

fn create_item(db: Sqlite.Database, clock: Clock, counter: Counter, body: ItemBody) : Int
  ensure_table(db)
  case db.run(
    "INSERT INTO items (name, created_at) VALUES (?1, ?2)",
    [Sqlite.String(body.name), Sqlite.String(clock.now.to_text)]
  )
    Ok(_): counter.bump
    Error(_): stop "sql failed"
  end
end

fn list_items(db: Sqlite.Database, counter: Counter) : Text
  ensure_table(db)
  case db.query("SELECT name, created_at FROM items ORDER BY rowid ASC", [])
    Ok(rows): "#{counter.current}:#{rows.rows.size}"
    Error(e): e.to_text
  end
end

fn run_expire(_payload: Json, db: Sqlite.Database, clock: Clock) : Result(Unit, Text)
  ensure_table(db)
  case db.run("DELETE FROM items WHERE created_at < ?1", [Sqlite.String(clock.now.to_text)])
    Ok(_): Ok(Unit)
    Error(e): Error(e.to_text)
  end
end

fn post_item(app: ApiTest.App, name: Text) : Api.Response
  app.send(Api.post("/items").with_json(ItemBody(name: name)))
end

test "copies of ApiTest.App share Sqlite, Clock, and synthesized Actor state"
  sql = Sqlite.in_memory()
  start_time = Time.parse("2026-06-01T12:00:00Z") or stop "valid time"
  clock = Clock.fixed(start_time)

  app = ApiTest.start(Main, sql, clock)
  copy = app

  assert app.send(Api.get("/now")).text == "\"2026-06-01T12:00:00Z\""
  assert post_item(app, "first").text == "1"
  assert copy.send(Api.get("/items")).text == "\"1:1\""

  # Advancing the outer clock handle advances time for all subsequent app.send and app.run_job calls.
  clock.sleep(15.minutes)
  assert copy.send(Api.get("/now")).text == "\"2026-06-01T12:15:00Z\""
  assert post_item(copy, "second").text == "2"
  assert app.send(Api.get("/items")).text == "\"2:2\""

  # Running the background job sees the advanced clock and mutates the shared Sqlite DB.
  assert app.run_job("expire.items", Json.Null) == Ok(Unit)
  assert copy.send(Api.get("/items")).text == "\"2:1\""

  # Same helper works across multiple different api modules.
  other = ApiTest.start(OtherApi)
  assert Helpers.route_paths(app) == [
    "GET /now (none)",
    "POST /items (none)",
    "GET /items (none)"
  ]
  assert Helpers.route_paths(other) == ["GET /ping (none)"]
  assert Helpers.openapi_len(app) > 50
  assert Helpers.openapi_len(other) > 50
  assert other.send(Api.get("/ping")).text == "\"pong\""
end

test "ApiTest.start also works without parens on zero-arg methods"
  app = ApiTest.start(OtherApi)
  assert app.routes.size == 1
  assert app.openapi_json.contains?("\"/ping\"")
end
"##,
    )?;

    let out = run_mo(&["test", "main.mo"], &dir)?;
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    let _ = fs::remove_dir_all(&dir);
    assert!(
        out.status.success(),
        "expected mo test to succeed, status={:?}\nstdout:\n{stdout}\nstderr:\n{stderr}",
        out.status.code()
    );
    Ok(())
}

#[test]
fn apitest_app_rejects_outside_spec_main_service_actor_abilities_capture_and_promise_input()
-> TestResult {
    let cases: &[(&str, &str, &str)] = &[
        (
            "outside-spec",
            r#"module Main
api
  port 8080
  get "/ok" ok_handler
end
fn ok_handler() : Text
  "ok"
end
fn bad_helper() : ApiTest.App
  ApiTest.start(Main)
end
"#,
            "E0098",
        ),
        (
            "on-main",
            r#"module Main
fn main(_app: ApiTest.App)
  Unit
end
"#,
            "E0079",
        ),
        (
            "service-state",
            r#"module Main
struct Holder
  app: Option(ApiTest.App)
end
service
  state Holder = Holder(app: None)
  tools ping
  token from env "TOK"
end
fn ping(_s: Holder, _now: Time) : Int
  0
end
"#,
            "E0104",
        ),
        (
            "actor-state",
            r#"module Main
actor BadActor
  state Option(ApiTest.App) = None
  calls ping
end
fn ping(_s: Option(ApiTest.App), _now: Time) : Int
  0
end
"#,
            "E0104",
        ),
        (
            "ability-equal",
            r#"module Main
fn same?(a: ApiTest.App, b: ApiTest.App) : Bool
  a == b
end
"#,
            "E0080",
        ),
        (
            "lambda-capture",
            r#"module Main
use Api
fn make_sender(app: ApiTest.App) : fn(Api.Request) : Api.Response
  fn(req) app.send(req) end
end
"#,
            "E0091",
        ),
    ];

    for (tag, src, expected_code) in cases {
        let dir = unique_temp_dir(tag)?;
        fs::write(dir.join("main.mo"), src)?;
        let out = run_mo(&["check", "main.mo"], &dir)?;
        let combined = format!(
            "{}{}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        let _ = fs::remove_dir_all(&dir);
        assert!(
            !out.status.success(),
            "case {tag} unexpectedly succeeded:\n{combined}"
        );
        assert!(
            combined.contains(expected_code),
            "case {tag} expected {expected_code}, got:\n{combined}"
        );
    }

    // Promise generated parameter of type ApiTest.App must be rejected with E1400 in `mo test`.
    let dir = unique_temp_dir("promise-input")?;
    fs::write(
        dir.join("main.mo"),
        r#"module Main
fn count_routes(app: ApiTest.App) : Int
  promise
    result >= 0
  end

  app.routes().size
end
"#,
    )?;
    let out = run_mo(&["test", "main.mo"], &dir)?;
    let combined = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    let _ = fs::remove_dir_all(&dir);
    assert!(
        !out.status.success(),
        "promise-input unexpectedly succeeded:\n{combined}"
    );
    assert!(
        combined.contains("E1400"),
        "promise-input expected E1400, got:\n{combined}"
    );
    Ok(())
}

#[test]
fn apitest_large_route_table_scales_past_250_routes() -> TestResult {
    let dir = unique_temp_dir("many-routes")?;
    let mut routes = String::new();
    let mut handlers = String::new();
    let total_routes = 260;

    for i in 0..total_routes {
        let (method, suffix, has_id) = match i % 3 {
            0 => ("get", "", false),
            1 => ("post", "/:id", true),
            _ => ("delete", "/:id", true),
        };
        routes.push_str(&format!("  {method} \"/r/{i}{suffix}\" h_{i}\n"));
        if has_id {
            handlers.push_str(&format!(
                "fn h_{i}(_env: Env, _clock: Clock, _files: Files, id: Text) : Text\n  \"route:{i}:#{{id}}\"\nend\n"
            ));
        } else {
            handlers.push_str(&format!(
                "fn h_{i}(_env: Env, _clock: Clock, _files: Files) : Text\n  \"route:{i}\"\nend\n"
            ));
        }
    }

    let src = format!(
        r##"module Main
use Api

api
  port 8099
{routes}end

{handlers}
test "dispatches cleanly across 260 routes without E1203"
  files = Files.in_memory({{}})
  clock = Clock.fixed(Time.parse("2026-11-26T00:00:00Z") or stop "clock")
  env = Env.of({{}})

  r0 = ApiTest.send(Main, Api.get("/r/0"), files, clock, env)
  assert r0.status == 200 and r0.text == "\"route:0\""

  r_mid = ApiTest.send(Main, Api.post("/r/130/item-130"), files, clock, env)
  assert r_mid.status == 200 and r_mid.text == "\"route:130:item-130\""

  r_del = ApiTest.send(Main, Api.delete("/r/257/item-257"), files, clock, env)
  assert r_del.status == 200 and r_del.text == "\"route:257:item-257\""

  r_last = ApiTest.send(Main, Api.post("/r/259/item-259"), files, clock, env)
  assert r_last.status == 200 and r_last.text == "\"route:259:item-259\""

  r_405 = ApiTest.send(Main, Api.request("PUT", "/r/0"), files, clock, env)
  assert r_405.status == 405

  r_404 = ApiTest.send(Main, Api.get("/missing"), files, clock, env)
  assert r_404.status == 404

  r_oa = ApiTest.send(Main, Api.get("/openapi.json"), files, clock, env)
  assert r_oa.status == 200 and r_oa.text.contains?("\"/r/259/")

  r_docs = ApiTest.send(Main, Api.get("/docs"), files, clock, env)
  assert r_docs.status == 200
end
"##
    );

    fs::write(dir.join("main.mo"), src)?;
    let out = run_mo(&["test", "main.mo"], &dir)?;
    let combined = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    let _ = fs::remove_dir_all(&dir);
    assert!(
        out.status.success(),
        "260-route ApiTest failed:\n{combined}"
    );
    assert!(
        combined.contains("1 passed, 0 failed, 0 stopped"),
        "unexpected test output:\n{combined}"
    );
    Ok(())
}
