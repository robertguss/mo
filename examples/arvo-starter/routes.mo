## Route handlers, domain types, migrations, and auth adapter for the Arvo + Inertia v2 starter.
module Routes
expose Member(..),
Bill(..),
PayInput(..),
AppError(..),
authenticate,
show_login,
post_login,
post_logout,
home,
create_bill,
pay_bill,
import_csv
use Api
use Arvo.Audit
use Arvo.Auth
use Arvo.Changeset
use Arvo.Flash
use Arvo.Form
use Arvo.Inertia
use Arvo.Migrations
use Arvo.Session
use Json

## Authenticated household member + active SQLite-backed session.
struct Member
  id: Text
  email: Text
  display_name: Text
  session: Session.Session
end

struct MemberRow
  id: Text
  email: Text
  password_hash: Text
  display_name: Text
end

## One bill row in the starter ledger, carrying an optimistic concurrency `version`.
struct Bill
  id: Int
  name: Text
  category: Text
  amount_cents: Int
  status: Text
  version: Int
end

struct NewBillDraft
  name: Text
  category: Text
  amount_cents: Int
end

## JSON payload for marking a bill `in_flight` with an optimistic concurrency check.
struct PayInput
  expected_version: Int
  reason: Option(Text)
end

## Domain errors mapped to HTTP status codes in `Main`'s `api` block.
enum AppError
  Unauthenticated
  Forbidden
  BillNotFound(id: Int)
  VersionConflict(id: Int)
  BadUpload(reason: Text)
  ServerFailed(reason: Text)
end

default_dev_key_hex = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"

fn app_secret_key(env: Env) : Result(Bytes, AppError)
  hex = env.get("ARVO_SECRET_KEY") or default_dev_key_hex
  case Bytes.from_hex(hex)
    Some(k):
      if k.size == 32
        Ok(k)
      else
        Error(ServerFailed(reason: "ARVO_SECRET_KEY must decode to 32 bytes"))
      end
    None: Error(ServerFailed(reason: "ARVO_SECRET_KEY must be 64 hex characters"))
  end
end

fn allowed_emails(env: Env) : List(Text)
  raw = env.get("ARVO_ALLOWLIST") or "ada@household.local,grace@household.local"
  raw.split(",").map(fn(s) s.trim end).filter(fn(s) not s.empty? end)
end

fn secure_cookies?(env: Env) : Bool
  (env.get("ARVO_INSECURE_COOKIES") or "0") != "1"
end

fn session_cfg(env: Env) : Session.SessionConfig
  Session.SessionConfig(
    cookie_name: "arvo_session",
    ttl_seconds: 1209600,
    secure?: secure_cookies?(env)
  )
end

fn app_migrations() : List(Migrations.Migration)
  m1_sql = """
    CREATE TABLE IF NOT EXISTS members (
      id TEXT PRIMARY KEY,
      email TEXT NOT NULL UNIQUE,
      password_hash TEXT NOT NULL,
      display_name TEXT NOT NULL,
      created_at TEXT NOT NULL
    ) STRICT;
    #{Session.schema_sql()}
    #{Audit.schema_sql()}
    CREATE TABLE IF NOT EXISTS bills (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      name TEXT NOT NULL,
      category TEXT NOT NULL,
      amount_cents INTEGER NOT NULL CHECK (amount_cents > 0),
      status TEXT NOT NULL DEFAULT 'due' CHECK (status IN ('due', 'in_flight', 'paid')),
      version INTEGER NOT NULL DEFAULT 1,
      created_at TEXT NOT NULL
    ) STRICT;
    """
  [Migrations.Migration(version: 1, name: "init_arvo_starter", sql: m1_sql)]
end

fn bootstrap_db(db: Sqlite.Database, rng: SecureRandom, clock: Clock, env: Env) : Result(
  Unit,
  AppError
)
  _applied = try Migrations.run(db, clock, app_migrations()).map_error(
    fn(e) ServerFailed(reason: "migration failed: #{e}") end
  )
  count_rows = try db.query("SELECT id FROM members LIMIT 1", []).map_error(
    fn(e) ServerFailed(reason: "#{e}") end
  )
  if count_rows.rows.empty?
    seed_pw = env.get("ARVO_SEED_PASSWORD") or "correct-horse-battery-staple"
    pw_hash = try Auth.hash_password(rng, seed_pw).map_error(
      fn(e) ServerFailed(reason: "password hash failed: #{e}") end
    )
    now_iso = clock.now.to_text
    _m = try db.execute(
      "INSERT INTO members (id, email, password_hash, display_name, created_at) VALUES (?, ?, ?, ?, ?)",
      [
        Sqlite.String("mem_ada"),
        Sqlite.String("ada@household.local"),
        Sqlite.String(pw_hash),
        Sqlite.String("Ada & Household <Team>"),
        Sqlite.String(now_iso)
      ]
    ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
    _b = try db.execute(
      "INSERT INTO bills (name, category, amount_cents, status, version, created_at) VALUES (?, ?, ?, 'due', 1, ?)",
      [
        Sqlite.String("Electric & Gas <Utility>"),
        Sqlite.String("utilities"),
        Sqlite.Integer(14500),
        Sqlite.String(now_iso)
      ]
    ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
  end
  Ok(Unit)
end

fn load_inertia_config(files: ReadFiles, env: Env) : Result(Inertia.InertiaConfig, AppError)
  title = "Arvo Starter"
  head = "<meta name=\"theme-color\" content=\"#0f172a\">"
  if env.get("VITE_DEV_URL") is Some(dev_url)
    if not dev_url.trim.empty?
      return Ok(Inertia.dev_config(title, dev_url.trim, "src/main.tsx", head))
    end
  end
  case files.read_bytes("public/manifest.json")
    Ok(manifest_bytes): Inertia.config_from_manifest(title, manifest_bytes, "src/main.tsx", head)
      .map_error(fn(reason) ServerFailed(reason: reason) end)
    Error(_): Ok(Inertia.dev_config(title, "http://127.0.0.1:5173", "src/main.tsx", head))
  end
end

fn shared_props(viewer: Option(Member), flash: Flash.FlashData) : Map(Text, Json)
  var base = {
    "errors": Changeset.inertia_error_map_json(flash.errors),
    "flash": Json.Object({"notice": flash.notice.to_json, "alert": flash.alert.to_json})
  }
  case viewer
    Some(m): base = base.set(
      "auth",
      Json.Object({
        "id": Json.String(m.id),
        "email": Json.String(m.email),
        "display_name": Json.String(m.display_name)
      })
    )
    None: base = base.set("auth", Json.Null)
  end
  base
end

## Authenticates the encrypted `arvo_session` cookie and loads the corresponding `Member`.
fn authenticate(req: Api.Request, db: Sqlite.Database, rng: SecureRandom, clock: Clock, env: Env) : Result(
  Member,
  AppError
)
  try bootstrap_db(db, rng, clock, env)
  key = try app_secret_key(env)
  maybe_sess = try Session.load(req, db, clock, key, session_cfg(env)).map_error(
    fn(e) ServerFailed(reason: "session load failed: #{e}") end
  )
  sess = try maybe_sess.or_fail(Unauthenticated)
  rows = try db.query(
    "SELECT id, email, password_hash, display_name FROM members WHERE id = ?",
    [Sqlite.String(sess.member_id)]
  ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
  maybe_member = try Sqlite.decode_optional[MemberRow](rows).map_error(
    fn(e) ServerFailed(reason: e.to_text) end
  )
  mrow = try maybe_member.or_fail(Unauthenticated)
  Ok(Member(id: mrow.id, email: mrow.email, display_name: mrow.display_name, session: sess))
end

## Renders the Inertia `Login` page for guests, or redirects authenticated members to `/`.
fn show_login(req: Api.Request, viewer: Option(Member), files: ReadFiles, env: Env) : Result(
  Api.Response,
  AppError
)
  if viewer is Some(_)
    return Ok(Inertia.redirect("/"))
  end
  cfg = try load_inertia_config(files, env)
  if Inertia.check_version(req, cfg) is Some(conflict)
    return Ok(conflict)
  end
  key = try app_secret_key(env)
  (flash, had_flash?) = Flash.take(req, key)
  res = Inertia.render(
    req,
    cfg,
    "Login",
    {"allowlist_hint": Json.String("Household members only")},
    shared_props(None, flash)
  )
  Ok(Flash.clear_if_present(res, had_flash?, secure_cookies?(env)))
end

## Verifies allowlisted email + scrypt password, issues encrypted session + CSRF cookies, and redirects `303`.
fn post_login(req: Api.Request, db: Sqlite.Database, rng: SecureRandom, clock: Clock, env: Env) : Result(
  Api.Response,
  AppError
)
  try bootstrap_db(db, rng, clock, env)
  key = try app_secret_key(env)
  sec? = secure_cookies?(env)
  fields = try Form.parse_urlencoded(req).map_error(
    fn(e) BadUpload(reason: "invalid login form: #{e}") end
  )
  email = Auth.normalize_email(Form.field(fields, "email") or "")
  password = Form.field(fields, "password") or ""

  if not Auth.email_allowed?(email, allowed_emails(env))
    return Ok(
      Inertia.validation_error(
        req,
        rng,
        key,
        "/login",
        {"email": ["email is not on the household allowlist"]},
        sec?
      )
    )
  end

  rows = try db.query(
    "SELECT id, email, password_hash, display_name FROM members WHERE email = ?",
    [Sqlite.String(email)]
  ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
  maybe_member = try Sqlite.decode_optional[MemberRow](rows).map_error(
    fn(e) ServerFailed(reason: e.to_text) end
  )
  mrow = case maybe_member
    Some(m): m
    None: return Ok(
      Inertia.validation_error(
        req,
        rng,
        key,
        "/login",
        {"email": ["invalid email or password"]},
        sec?
      )
    )
  end

  if not Auth.verify_password(password, mrow.password_hash)
    return Ok(
      Inertia.validation_error(
        req,
        rng,
        key,
        "/login",
        {"password": ["invalid email or password"]},
        sec?
      )
    )
  end

  scfg = session_cfg(env)
  (sess, cookie_val) = try Session.create(db, rng, clock, key, mrow.id, scfg).map_error(
    fn(e) ServerFailed(reason: "session create failed: #{e}") end
  )
  _ev = try Audit.record(
    db,
    rng,
    clock,
    mrow.email,
    "auth.login",
    "members",
    mrow.id,
    None,
    Some({"session_id": sess.id}.to_json),
    None,
    None
  ).map_error(fn(e) ServerFailed(reason: "#{e}") end)

  redir = Flash.attach(Inertia.redirect("/"), rng, key, Flash.with_notice("Signed in"), sec?)
  Ok(Auth.login_response(redir, sess, cookie_val, scfg))
end

## Enforces CSRF, revokes the active SQLite session row, clears both cookies, and redirects to `/login`.
fn post_logout(req: Api.Request, member: Member, db: Sqlite.Database, env: Env) : Result(
  Api.Response,
  AppError
)
  return Error(Forbidden) if not Auth.require_csrf(req, member.session)
  _rev = try Session.revoke(db, member.session.id).map_error(fn(e) ServerFailed(reason: "#{e}") end)
  Ok(Auth.logout_response(Inertia.redirect("/login"), session_cfg(env)))
end

## Renders the household `Home` Inertia dashboard (or redirects unauthenticated browsers to `/login`).
fn home(
  req: Api.Request,
  viewer: Option(Member),
  db: Sqlite.Database,
  files: ReadFiles,
  env: Env,
  status: Option(Text)
) : Result(Api.Response, AppError)
  member = case viewer
    Some(m): m
    None: return Ok(Inertia.redirect("/login"))
  end

  cfg = try load_inertia_config(files, env)
  if Inertia.check_version(req, cfg) is Some(conflict)
    return Ok(conflict)
  end

  bill_rows = case status
    Some(st): try db.query(
      "SELECT id, name, category, amount_cents, status, version FROM bills WHERE status = ? ORDER BY id ASC",
      [Sqlite.String(st)]
    ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
    None: try db.query(
      "SELECT id, name, category, amount_cents, status, version FROM bills ORDER BY id ASC",
      []
    ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
  end
  bills = try Sqlite.decode[Bill](bill_rows).map_error(fn(e) ServerFailed(reason: e.to_text) end)
  audit_feed = try Audit.list_recent(db, 20).map_error(
    fn(e) ServerFailed(reason: "audit query failed: #{e}") end
  )

  key = try app_secret_key(env)
  (flash, had_flash?) = Flash.take(req, key)
  total_due_cents = bills.filter(fn(b) b.status == "due" end).map(fn(b) b.amount_cents end).sum

  res = Inertia.render(
    req,
    cfg,
    "Home",
    {
      "bills": bills.to_json,
      "total_due_cents": Json.int(total_due_cents),
      "audit_feed": audit_feed.to_json
    },
    shared_props(Some(member), flash)
  )
  Ok(Flash.clear_if_present(res, had_flash?, secure_cookies?(env)))
end

## Validates a new bill via `Arvo.Changeset`, persisting on valid input or flashing errors across `303`.
fn create_bill(
  req: Api.Request,
  member: Member,
  db: Sqlite.Database,
  rng: SecureRandom,
  clock: Clock,
  env: Env
) : Result(Api.Response, AppError)
  return Error(Forbidden) if not Auth.require_csrf(req, member.session)
  fields = try Form.parse_urlencoded(req).map_error(
    fn(e) BadUpload(reason: "invalid bill form: #{e}") end
  )
  name = (Form.field(fields, "name") or "").trim
  category = (Form.field(fields, "category") or "").trim
  raw_amount = (Form.field(fields, "amount_cents") or "").trim
  amount_cents = raw_amount.to_int or -1

  draft = NewBillDraft(name: name, category: category, amount_cents: amount_cents)
  cs = Changeset.new(draft).require_non_empty("name", draft.name).require_length(
    "name",
    draft.name,
    2,
    80
  ).require_one_of("category", draft.category, ["utilities", "housing", "groceries", "card"])
    .require_positive_int("amount_cents", draft.amount_cents)

  key = try app_secret_key(env)
  sec? = secure_cookies?(env)
  validated = case Changeset.finish(cs)
    Ok(v): v
    Error(errs): return Ok(Inertia.validation_error(req, rng, key, "/", errs, sec?))
  end

  ins = try db.execute(
    "INSERT INTO bills (name, category, amount_cents, status, version, created_at) VALUES (?, ?, ?, 'due', 1, ?)",
    [
      Sqlite.String(validated.name),
      Sqlite.String(validated.category),
      Sqlite.Integer(validated.amount_cents),
      Sqlite.String(clock.now.to_text)
    ]
  ).map_error(fn(e) ServerFailed(reason: "#{e}") end)

  created = Bill(
    id: ins.last_insert_rowid,
    name: validated.name,
    category: validated.category,
    amount_cents: validated.amount_cents,
    status: "due",
    version: 1
  )
  _ev = try Audit.record(
    db,
    rng,
    clock,
    member.email,
    "bill.create",
    "bills",
    "#{created.id}",
    None,
    Some(created.to_json),
    None,
    None
  ).map_error(fn(e) ServerFailed(reason: "#{e}") end)

  Ok(Flash.attach(Inertia.redirect("/"), rng, key, Flash.with_notice("Bill added"), sec?))
end

## Marks a bill `in_flight` ("I just paid this") with optimistic concurrency + `Arvo.Audit.record`.
fn pay_bill(
  req: Api.Request,
  member: Member,
  db: Sqlite.Database,
  rng: SecureRandom,
  clock: Clock,
  id: Int,
  input: PayInput
) : Result(Bill, AppError)
  return Error(Forbidden) if not Auth.require_csrf(req, member.session)
  rows = try db.query(
    "SELECT id, name, category, amount_cents, status, version FROM bills WHERE id = ?",
    [Sqlite.Integer(id)]
  ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
  existing = try Sqlite.decode_optional[Bill](rows).map_error(
    fn(e) ServerFailed(reason: e.to_text) end
  )
  before_bill = try existing.or_fail(BillNotFound(id: id))

  upd = try db.execute(
    "UPDATE bills SET status = 'in_flight', version = version + 1 WHERE id = ? AND version = ?",
    [Sqlite.Integer(id), Sqlite.Integer(input.expected_version)]
  ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
  try Audit.ensure_updated(upd).map_error(fn(_) VersionConflict(id: id) end)

  after_bill = Bill(
    id: before_bill.id,
    name: before_bill.name,
    category: before_bill.category,
    amount_cents: before_bill.amount_cents,
    status: "in_flight",
    version: before_bill.version + 1
  )
  _ev = try Audit.record(
    db,
    rng,
    clock,
    member.email,
    "bill.mark_in_flight",
    "bills",
    "#{id}",
    Some(before_bill.to_json),
    Some(after_bill.to_json),
    input.reason,
    Some(
      Json.Object({
        "restore_status": Json.String(before_bill.status),
        "restore_version": Json.int(before_bill.version)
      })
    )
  ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
  Ok(after_bill)
end

## Parses a multipart CSV upload (`name,category,amount_cents` rows), inserts bills, and logs an audit event.
fn import_csv(
  req: Api.Request,
  member: Member,
  db: Sqlite.Database,
  rng: SecureRandom,
  clock: Clock
) : Result(Map(Text, Int), AppError)
  return Error(Forbidden) if not Auth.require_csrf(req, member.session)
  mp = try Form.parse_multipart(req, 65536).map_error(
    fn(e) BadUpload(reason: "multipart error: #{e}") end
  )
  upload = try Form.file(mp, "statement").or_fail(BadUpload(reason: "missing 'statement' file"))
  csv_text = try upload.data.decode_utf8.or_fail(BadUpload(reason: "CSV is not valid UTF-8"))
  default_cat = Form.field(mp.fields, "default_category") or "utilities"

  var imported = 0
  now_iso = clock.now.to_text
  for (idx, raw_line) in csv_text.lines.enumerate
    line = raw_line.trim
    continue if line.empty?
    continue if idx == 0 and line.ascii_lowercase.starts_with?("name,")
    cols = line.split(",")
    if cols.size < 3
      return Error(BadUpload(reason: "expected 3 CSV columns at line #{idx + 1}"))
    end
    bname = (cols.get(0) or "").trim
    bcat_raw = (cols.get(1) or "").trim
    bcat = if bcat_raw.empty?: default_cat else: bcat_raw
    cents = try (cols.get(2) or "").trim.to_int.or_fail(
      BadUpload(reason: "invalid amount_cents at line #{idx + 1}")
    )
    if bname.empty? or cents <= 0
      return Error(BadUpload(reason: "invalid bill row at line #{idx + 1}"))
    end
    _ins = try db.execute(
      "INSERT INTO bills (name, category, amount_cents, status, version, created_at) VALUES (?, ?, ?, 'due', 1, ?)",
      [Sqlite.String(bname), Sqlite.String(bcat), Sqlite.Integer(cents), Sqlite.String(now_iso)]
    ).map_error(fn(e) ServerFailed(reason: "#{e}") end)
    imported += 1
  end

  _ev = try Audit.record(
    db,
    rng,
    clock,
    member.email,
    "bills.csv_import",
    "bills",
    upload.filename,
    None,
    Some({"filename": upload.filename, "imported": imported.to_text}.to_json),
    Some("Multipart CSV upload"),
    None
  ).map_error(fn(e) ServerFailed(reason: "#{e}") end)

  Ok({"imported": imported})
end
