//! Integration tests for `Sqlite.QueryStats`, `db.stats()`, and `db.reset_stats()`.
//!
//! Verifies:
//! 1. Positive: `db.stats()` and `db.reset_stats()` track `query_count`, `exec_count`, and
//!    parameterized SQL templates across direct queries, helper functions, and `ApiTest.start`
//!    route dispatch without ever recording bound parameter values/secrets.
//! 2. Able-to-fail N+1 budget assertion: a naive N+1 route fails an `expect_max_queries` budget,
//!    while a batched `JOIN` / `IN` route passes the same budget.
//! 3. Negative (compile-time): `db.stats()` / `db.reset_stats()` directly inside `fn main(...)`
//!    is rejected with `E0098`.
//! 4. Negative (runtime): calling a helper that invokes `db.stats()` or `db.reset_stats()` on a
//!    real production `Sqlite.Database` in `mo run` stops with exit code 70.

use std::error::Error;
use std::fs;
use std::path::PathBuf;
use std::process::Command;
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_ID: AtomicU64 = AtomicU64::new(1);

fn temp_dir(tag: &str) -> Result<PathBuf, Box<dyn Error>> {
    let n = NEXT_ID.fetch_add(1, Ordering::Relaxed);
    let dir =
        std::env::temp_dir().join(format!("mo-sqlite-stats-{tag}-{}-{n}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir)?;
    Ok(dir)
}

#[test]
fn sqlite_stats_tracks_apitest_route_queries_and_catches_n_plus_1() -> Result<(), Box<dyn Error>> {
    let mo = env!("CARGO_BIN_EXE_mo");
    let dir = temp_dir("n-plus-1")?;
    let main_mo = dir.join("main.mo");
    fs::write(
        &main_mo,
        r##"module Main
use Api

struct ItemRow
  id: Int
  title: Text
end

fn must_ok(res: Result(A, E)) : A
  case res
    Ok(v): v
    Error(_): stop "unexpected sqlite error"
  end
end

fn expect_max_queries(db: Sqlite.Database, max_queries: Int) : Bool
  db.stats().query_count <= max_queries
end

fn handle_batched(req: Api.Request, db: Sqlite.Database) : Api.Response
  _ = req
  rows = must_ok(db.query("SELECT id, title FROM items ORDER BY id", []))
  Api.text_response(200, "#{rows.rows.size}")
end

fn handle_n_plus_1(req: Api.Request, db: Sqlite.Database) : Api.Response
  _ = req
  ids = must_ok(db.query("SELECT id FROM items ORDER BY id", []))
  var count = 0
  for r in ids.rows
    id_cell = r.get(0) or Sqlite.Integer(0)
    _detail = must_ok(db.query("SELECT title FROM items WHERE id = ?", [id_cell]))
    count = count + 1
  end
  Api.text_response(200, "#{count}")
end

api
  sqlite db from "app.db"
  get "/items/batched" handle_batched
  get "/items/n-plus-1" handle_n_plus_1
end

test "N+1 query budget catches per-row loop and passes batched query across ApiTest.start"
  sql = Sqlite.in_memory()
  db = must_ok(sql.open("app.db"))
  _ = must_ok(
    db.run(
      "CREATE TABLE items(id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, secret TEXT NOT NULL)",
      []
    )
  )
  for i in 1..6
    _ = must_ok(
      db.execute(
        "INSERT INTO items(title, secret) VALUES (?, ?)",
        [Sqlite.String("item-#{i}"), Sqlite.String("bound-secret-#{i}-xyz")]
      )
    )
  end

  app = ApiTest.start(Main, sql)

  db.reset_stats()
  res_batched = app.send(Api.get("/items/batched"))
  assert res_batched.status == 200
  assert res_batched.text == "5"
  assert expect_max_queries(db, 2)
  assert db.stats().query_count == 1
  assert db.stats().exec_count == 0
  assert db.stats().statements == ["SELECT id, title FROM items ORDER BY id"]

  db.reset_stats()
  res_naive = app.send(Api.get("/items/n-plus-1"))
  assert res_naive.status == 200
  assert res_naive.text == "5"
  # 1 list query + 5 per-row detail queries = 6 queries (> budget of 2)
  assert db.stats().query_count == 6
  assert not expect_max_queries(db, 2)
  for stmt in db.stats().statements
    assert not stmt.contains?("bound-secret-")
  end
end
"##,
    )?;

    let out = Command::new(mo).arg("test").arg(&main_mo).output()?;
    assert!(
        out.status.success(),
        "expected mo test to succeed:\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr),
    );
    let _ = fs::remove_dir_all(&dir);
    Ok(())
}

#[test]
fn sqlite_stats_rejected_in_main_and_on_real_database_at_runtime() -> Result<(), Box<dyn Error>> {
    let mo = env!("CARGO_BIN_EXE_mo");
    let dir = temp_dir("reject")?;

    // 1. Direct use in `fn main` is rejected at compile time with E0098.
    let direct_main = dir.join("direct.mo");
    fs::write(
        &direct_main,
        r#"module Direct

fn main(sql: Sqlite)
  case sql.open("prod.db")
    Ok(db):
      _ = db.stats()
      Unit
    Error(_): Unit
  end
end
"#,
    )?;
    let check_out = Command::new(mo).arg("check").arg(&direct_main).output()?;
    assert_eq!(check_out.status.code(), Some(1));
    let stderr = String::from_utf8_lossy(&check_out.stderr);
    assert!(
        stderr.contains("E0098"),
        "expected E0098 when calling db.stats() in main, got:\n{stderr}"
    );

    // 2. Passing a real production `Sqlite.Database` to a helper that calls `db.stats()`
    //    stops at runtime with exit code 70 and zero production stats state.
    let runtime_dir = temp_dir("runtime-reject")?;
    let runtime_main = runtime_dir.join("main.mo");
    fs::write(
        &runtime_main,
        r##"module Main

fn inspect_helper(db: Sqlite.Database) : Int
  db.stats().query_count
end

fn main(sql: Sqlite, console: Console)
  case sql.open("real.db")
    Ok(db):
      n = inspect_helper(db)
      console.print("#{n}")
    Error(_):
      console.print("open failed")
  end
end
"##,
    )?;
    let run_out = Command::new(mo)
        .current_dir(&runtime_dir)
        .arg("run")
        .arg(&runtime_main)
        .output()?;
    assert_eq!(
        run_out.status.code(),
        Some(70),
        "expected exit code 70 on real Sqlite.Database.stats:\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&run_out.stdout),
        String::from_utf8_lossy(&run_out.stderr),
    );
    let run_stderr = String::from_utf8_lossy(&run_out.stderr);
    assert!(
        run_stderr.contains(
            "Sqlite.Database.stats is only available on fake or in-memory test databases"
        ),
        "expected clear bug reason on stderr, got:\n{run_stderr}"
    );

    // 3. Same for `db.reset_stats()` on a real production `Sqlite.Database`.
    let reset_dir = temp_dir("runtime-reset-reject")?;
    let reset_main = reset_dir.join("main.mo");
    fs::write(
        &reset_main,
        r#"module Main

fn reset_helper(db: Sqlite.Database) : Unit
  db.reset_stats()
end

fn main(sql: Sqlite)
  case sql.open(":memory:")
    Ok(db): reset_helper(db)
    Error(_): Unit
  end
end
"#,
    )?;
    let reset_out = Command::new(mo)
        .current_dir(&reset_dir)
        .arg("run")
        .arg(&reset_main)
        .output()?;
    assert_eq!(
        reset_out.status.code(),
        Some(70),
        "expected exit code 70 on real Sqlite.Database.reset_stats:\nstderr:\n{}",
        String::from_utf8_lossy(&reset_out.stderr),
    );

    let _ = fs::remove_dir_all(&dir);
    let _ = fs::remove_dir_all(&runtime_dir);
    let _ = fs::remove_dir_all(&reset_dir);
    Ok(())
}
