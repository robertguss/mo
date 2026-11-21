## Autonomous coding-agent harness in Mo (`mo-harness`): workspace-confined `Files.within`
## and `Command` (`mo`, `git`), host-allowlisted `Http` gateway turns, `TaskCoordinator`
## BEAM actor budget/subagent orchestration, and replayable `Sqlite` trajectories.
module Main
use Command
use Coordinator{TaskCoordinator, RunStatus, BudgetError, SubagentRecord}
use Http
use Json

enum ToolCall
  ReadFile(path: Text)
  WriteFile(path: Text, content: Text)
  ListFiles(folder: Text)
  MoCheck(file: Text)
  MoTest(file: Text)
  MoDiff(file: Text)
  GitStatus
  RunProgram(program: Text, args: List(Text))
  DelegateSubagent(role: Text, target_file: Text)
end

enum AgentAction
  CallTool(ToolCall)
  Complete(summary: Text)
end

struct ModelTurn
  thought: Text
  input_tokens: Int
  output_tokens: Int
  action: AgentAction
end

struct Message
  role: Text
  content: Text
end

struct TurnRequest
  model: Text
  goal: Text
  step: Int
  messages: List(Message)
end

struct TurnLog
  step: Int
  thought: Text
  action_json: Text
  observation: Text
  spent_usd: Decimal
end

struct RunReport
  run_id: Text
  completed: Bool
  summary: Text
  steps: Int
  spent_usd: Decimal
  subagents: List(SubagentRecord)
  turns: List(TurnLog)
end

enum HarnessError
  DbFailure(SqliteError)
  GatewayHttp(Http.HttpError)
  GatewayStatus(status: Int)
  MalformedTurn(reason: Text)
  BudgetStopped(BudgetError)
  StepCapExhausted(steps: Int)
end

struct WorkspaceCaps
  files: Files
  command: Command
  network: Network
end

## Checks whether a requested tool binary is on the harness's static allowlist (`mo` or `git`).
fn allowed_tool_program?(program: Text) : Bool
  promise "only plain mo and git basenames are permitted"
    (program.contains?("/") or program.contains?("..")) implies not result
    result implies (program == "mo" or program == "git")
  end
  examples
    allowed_tool_program?("mo")
    allowed_tool_program?("git")
    not allowed_tool_program?("curl")
    not allowed_tool_program?("../sh")
  end

  program == "mo" or program == "git"
end

## Narrows raw host capabilities to `workspace`, `["mo", "git"]`, and `gateway_host_port`.
fn confine_workspace(
  files: Files,
  command: Command,
  network: Network,
  workspace: Text,
  gateway_host_port: Text
) : WorkspaceCaps
  WorkspaceCaps(
    files: files.within(workspace),
    command: command.within(workspace).allow_programs(["mo", "git"]),
    network: network.allow_hosts([gateway_host_port])
  )
end

fn format_cmd_result(res: Result(Command.Output, Command.CommandError)) : Text
  case res
    Ok(out): "exit=#{out.status}\nstdout:\n#{out.stdout}stderr:\n#{out.stderr}"
    Error(Command.OutsideFolder(path: p)): "command_error: outside_folder #{p}"
    Error(Command.NotPermitted(program: p)): "command_error: not_permitted #{p}"
    Error(Command.TimedOut): "command_error: timed_out"
    Error(Command.OutputTooLarge): "command_error: output_too_large"
    Error(Command.SpawnFailed(reason: r)): "command_error: spawn_failed #{r}"
  end
end

## Executes a scoped read-only research subagent over `files.read_only` and records its summary.
fn run_subagent(read_files: ReadFiles, coordinator: TaskCoordinator, role: Text, target_file: Text) : Text
  summary = case read_files.read_text(target_file)
    Ok(text):
      lines = text.lines
      preview = lines.first or ""
      "inspected #{target_file} (#{lines.size} lines, head: #{preview})"
    Error(err): "read_failed #{target_file}: #{err}"
  end
  case coordinator.record_subagent(role, summary)
    Ok(rec): "subagent##{rec.id} [#{rec.role}]: #{rec.summary}"
    Error(err): "subagent_refused: #{err}"
  end
end

## Executes one model-requested tool strictly through the attenuated workspace capabilities.
fn execute_tool(caps: WorkspaceCaps, coordinator: TaskCoordinator, call: ToolCall) : Text
  case call
    ReadFile(path: path):
      case caps.files.read_text(path)
        Ok(content): "ok:\n#{content}"
        Error(err): "file_error: #{err}"
      end
    WriteFile(path: path, content: content):
      case caps.files.write_text(path, content)
        Ok(Unit): "wrote #{content.byte_count} bytes to #{path}"
        Error(err): "file_error: #{err}"
      end
    ListFiles(folder: folder):
      case caps.files.list(folder)
        Ok(entries): "entries: #{entries.join(", ")}"
        Error(err): "file_error: #{err}"
      end
    MoCheck(file: file): format_cmd_result(caps.command.run("mo", ["check", file], 30.seconds))
    MoTest(file: file): format_cmd_result(caps.command.run("mo", ["test", file], 30.seconds))
    MoDiff(file: file): format_cmd_result(caps.command.run("mo", ["diff", file], 30.seconds))
    GitStatus: format_cmd_result(caps.command.run("git", ["status", "-s"], 10.seconds))
    RunProgram(program: program, args: args): format_cmd_result(
      caps.command.run(program, args, 15.seconds)
    )
    DelegateSubagent(role: role, target_file: target_file): run_subagent(
      caps.files.read_only,
      coordinator,
      role,
      target_file
    )
  end
end

fn init_db(db: Sqlite.Database) : Result(Unit, HarnessError)
  try db.run(
    "CREATE TABLE IF NOT EXISTS trajectories (run_id TEXT PRIMARY KEY, goal TEXT NOT NULL, completed INT NOT NULL, summary TEXT NOT NULL, steps INT NOT NULL, spent_usd TEXT NOT NULL)",
    []
  )
  try db.run(
    "CREATE TABLE IF NOT EXISTS turns (run_id TEXT NOT NULL, step INT NOT NULL, thought TEXT NOT NULL, action_json TEXT NOT NULL, observation TEXT NOT NULL, spent_usd TEXT NOT NULL, PRIMARY KEY (run_id, step))",
    []
  )
  Ok(Unit)
end

fn save_turn(db: Sqlite.Database, run_id: Text, turn: TurnLog) : Result(Unit, HarnessError)
  try db.run(
    "INSERT INTO turns (run_id, step, thought, action_json, observation, spent_usd) VALUES (?, ?, ?, ?, ?, ?)",
    [
      Sqlite.String(run_id),
      Sqlite.Integer(turn.step),
      Sqlite.String(turn.thought),
      Sqlite.String(turn.action_json),
      Sqlite.String(turn.observation),
      Sqlite.String(turn.spent_usd.to_fixed(6))
    ]
  )
  Ok(Unit)
end

fn save_trajectory(
  db: Sqlite.Database,
  run_id: Text,
  goal: Text,
  completed: Bool,
  summary: Text,
  steps: Int,
  spent_usd: Decimal
) : Result(Unit, HarnessError)
  flag = if completed: 1 else: 0
  try db.run(
    "INSERT OR REPLACE INTO trajectories (run_id, goal, completed, summary, steps, spent_usd) VALUES (?, ?, ?, ?, ?, ?)",
    [
      Sqlite.String(run_id),
      Sqlite.String(goal),
      Sqlite.Integer(flag),
      Sqlite.String(summary),
      Sqlite.Integer(steps),
      Sqlite.String(spent_usd.to_fixed(6))
    ]
  )
  Ok(Unit)
end

fn request_turn(network: Network, endpoint: Text, api_key: Text, req: TurnRequest) : Result(
  ModelTurn,
  HarnessError
)
  authed = Http.post(endpoint).header("Authorization", "Bearer #{api_key}")
  http_req = authed.json(req.to_json).within(30.seconds)
  resp = try http_req.send(network)
  return Error(GatewayStatus(status: resp.status)) if not resp.ok?
  body = try resp.text.or_fail(MalformedTurn(reason: "gateway body is not UTF-8"))
  case Json.decode_strict[ModelTurn](body)
    Ok(turn): Ok(turn)
    Error(err): Error(MalformedTurn(reason: "#{err}"))
  end
end

## Runs a full multi-turn coding-agent loop, persisting every step in `db` for deterministic replay.
fn run_agent(
  caps: WorkspaceCaps,
  db: Sqlite.Database,
  coordinator: TaskCoordinator,
  endpoint: Text,
  api_key: Text,
  run_id: Text,
  model: Text,
  goal: Text
) : Result(RunReport, HarnessError)
  try init_db(db)
  var messages = [Message(role: "user", content: goal)]
  var turns = []
  max_steps = coordinator.view.max_steps

  var step = 1
  while step <= max_steps
    req = TurnRequest(model: model, goal: goal, step: step, messages: messages)
    turn = try request_turn(caps.network, endpoint, api_key, req)
    snap = case coordinator.record_turn(turn.input_tokens, turn.output_tokens)
      Ok(view): view
      Error(berr):
        _stopped = coordinator.finish(false, "#{berr}")
        try save_trajectory(
          db,
          run_id,
          goal,
          false,
          "#{berr}",
          step - 1,
          coordinator.view.spent_usd
        )
        return Error(BudgetStopped(berr))
    end
    action_json = Json.encode(turn.action)
    case turn.action
      Complete(summary: summary):
        final_view = coordinator.finish(true, summary)
        done_log = TurnLog(
          step: step,
          thought: turn.thought,
          action_json: action_json,
          observation: "completed: #{summary}",
          spent_usd: final_view.spent_usd
        )
        turns = turns.append(done_log)
        try save_turn(db, run_id, done_log)
        try save_trajectory(db, run_id, goal, true, summary, final_view.steps, final_view.spent_usd)
        return Ok(
          RunReport(
            run_id: run_id,
            completed: true,
            summary: summary,
            steps: final_view.steps,
            spent_usd: final_view.spent_usd,
            subagents: final_view.subagents,
            turns: turns
          )
        )
      CallTool(tool):
        obs = execute_tool(caps, coordinator, tool)
        log = TurnLog(
          step: step,
          thought: turn.thought,
          action_json: action_json,
          observation: obs,
          spent_usd: snap.spent_usd
        )
        turns = turns.append(log)
        try save_turn(db, run_id, log)
        messages = messages.append(Message(role: "assistant", content: action_json))
        messages = messages.append(Message(role: "tool", content: obs))
    end
    step += 1
  end

  final_view = coordinator.finish(false, "step cap exhausted")
  try save_trajectory(
    db,
    run_id,
    goal,
    false,
    "step cap exhausted",
    final_view.steps,
    final_view.spent_usd
  )
  Error(StepCapExhausted(steps: final_view.steps))
end

## Reads recorded turns for `run_id` from SQLite so operators can inspect or audit every action.
fn load_turns(db: Sqlite.Database, run_id: Text) : Result(List(TurnLog), HarnessError)
  rows = try db.query(
    "SELECT step, thought, action_json, observation, spent_usd FROM turns WHERE run_id = ? ORDER BY step ASC",
    [Sqlite.String(run_id)]
  )
  var list = []
  for row in rows.rows
    if row is [
      Sqlite.Integer(step),
      Sqlite.String(thought),
      Sqlite.String(action_json),
      Sqlite.String(observation),
      Sqlite.String(spent_text)
    ]
      spent = spent_text.to_decimal or 0.00
      list = list.append(
        TurnLog(
          step: step,
          thought: thought,
          action_json: action_json,
          observation: observation,
          spent_usd: spent
        )
      )
    end
  end
  Ok(list)
end

fn parse_host_port(url: Text) : Text
  without_http = url.drop_prefix("http://") or (url.drop_prefix("https://") or url)
  parts = without_http.split("/")
  host_part = parts.first or "127.0.0.1:8080"
  if host_part.contains?(":")
    host_part
  else if url.starts_with?("https://")
    "#{host_part}:443"
  else
    "#{host_part}:80"
  end
end

fn main(
  args: Args,
  env: Env,
  console: Console,
  files: Files,
  command: Command,
  network: Network,
  sqlite: Sqlite,
  coordinator: TaskCoordinator
) : Int
  case args.list
    ["run", workspace, db_path, run_id, goal]:
      endpoint = env.get("MO_GATEWAY_URL") or "http://127.0.0.1:8080/v1/agent/turn"
      api_key = env.get("MO_GATEWAY_KEY") or "dev-key"
      model = env.get("MO_MODEL") or "claude-sonnet-4-6"
      caps = confine_workspace(files, command, network, workspace, parse_host_port(endpoint))
      case sqlite.open(db_path)
        Error(err):
          console.error("db open failed: #{err}")
          1
        Ok(db):
          case run_agent(caps, db, coordinator, endpoint, api_key, run_id, model, goal)
            Ok(report):
              console.print(
                "completed #{report.run_id} in #{report.steps} steps ($#{report.spent_usd.to_fixed(
                  6
                )}): #{report.summary}"
              )
              0
            Error(err):
              console.error("harness stopped: #{err}")
              1
          end
      end
    ["inspect", db_path, run_id]:
      case sqlite.open(db_path)
        Error(err):
          console.error("db open failed: #{err}")
          1
        Ok(db):
          case load_turns(db, run_id)
            Error(err):
              console.error("inspect failed: #{err}")
              1
            Ok(turns):
              for t in turns
                console.print(
                  "step #{t.step} ($#{t.spent_usd.to_fixed(6)}): #{t.thought} -> #{t.observation}"
                )
              end
              0
          end
      end
    _:
      console.error(
        "usage: mo run tools/mo-harness/main.mo (run <workspace> <db> <run-id> <goal> | inspect <db> <run-id>)"
      )
      2
  end
end

fn build_exchange(endpoint: Text, api_key: Text, req: TurnRequest, turn: ModelTurn) : (
  Http.Request,
  Http.Response
)
  authed = Http.post(endpoint).header("Authorization", "Bearer #{api_key}")
  asked = authed.json(req.to_json).within(30.seconds)
  (asked, Http.reply(200, Json.encode(turn)))
end

test "autonomous agent diagnoses a bug, delegates a subagent, self-corrects after mo test fails, and persists trajectory"
  endpoint = "http://gateway.internal:8080/v1/agent/turn"
  key = "sk-mo-test"
  goal = "Fix clamp.mo so negative numbers clamp to 0 and verify with mo test and mo diff."

  buggy_src = "module Clamp\nfn clamp_nonneg(n: Int) : Int\n  n\nend\n"
  bad_patch = "module Clamp\nfn clamp_nonneg(n: Int) : Int\n  if n < 0: -1 else: n\nend\n"
  good_patch = "module Clamp\nfn clamp_nonneg(n: Int) : Int\n  if n < 0: 0 else: n\nend\n"

  files = Files.in_memory({"repo/clamp.mo": buggy_src})
  cmd = Command.fake(
    Command.replies([
      (
        Command.spec("mo", ["test", "clamp.mo"]).in_folder("repo"),
        Command.output(1, "failed Clamp.test.1\n0 passed, 1 failed, 0 stopped\n", "")
      ),
      (Command.spec("mo", ["check", "clamp.mo"]).in_folder("repo"), Command.ok_output("")),
      (
        Command.spec("mo", ["diff", "clamp.mo"]).in_folder("repo"),
        Command.ok_output("contracts: 2 passed, 0 failed, 0 signature_changed, 0 removed\n")
      )
    ])
  )

  t1 = ModelTurn(
    thought: "Delegate a read-only scout subagent to inspect clamp.mo first.",
    input_tokens: 400,
    output_tokens: 100,
    action: CallTool(DelegateSubagent(role: "scout", target_file: "clamp.mo"))
  )
  t2 = ModelTurn(
    thought: "Write first candidate fix and run mo test.",
    input_tokens: 500,
    output_tokens: 120,
    action: CallTool(WriteFile(path: "clamp.mo", content: bad_patch))
  )
  t3 = ModelTurn(
    thought: "Run mo test on first candidate.",
    input_tokens: 550,
    output_tokens: 80,
    action: CallTool(MoTest(file: "clamp.mo"))
  )
  t4 = ModelTurn(
    thought: "mo test failed because negative numbers returned -1 instead of 0; patch to 0.",
    input_tokens: 700,
    output_tokens: 120,
    action: CallTool(WriteFile(path: "clamp.mo", content: good_patch))
  )
  t5 = ModelTurn(
    thought: "Verify semantic contract preservation with mo diff.",
    input_tokens: 800,
    output_tokens: 90,
    action: CallTool(MoDiff(file: "clamp.mo"))
  )
  t6 = ModelTurn(
    thought: "Contract diff passed; complete the run.",
    input_tokens: 850,
    output_tokens: 60,
    action: Complete(
      summary: "Fixed clamp_nonneg(n) to return 0 for n < 0 and verified with mo diff."
    )
  )

  m0 = [Message(role: "user", content: goal)]
  obs1 = "subagent#1 [scout]: inspected clamp.mo (4 lines, head: module Clamp)"
  m1 = m0.append(Message(role: "assistant", content: Json.encode(t1.action))).append(
    Message(role: "tool", content: obs1)
  )
  obs2 = "wrote #{bad_patch.byte_count} bytes to clamp.mo"
  m2 = m1.append(Message(role: "assistant", content: Json.encode(t2.action))).append(
    Message(role: "tool", content: obs2)
  )
  obs3 = "exit=1\nstdout:\nfailed Clamp.test.1\n0 passed, 1 failed, 0 stopped\nstderr:\n"
  m3 = m2.append(Message(role: "assistant", content: Json.encode(t3.action))).append(
    Message(role: "tool", content: obs3)
  )
  obs4 = "wrote #{good_patch.byte_count} bytes to clamp.mo"
  m4 = m3.append(Message(role: "assistant", content: Json.encode(t4.action))).append(
    Message(role: "tool", content: obs4)
  )
  obs5 = "exit=0\nstdout:\ncontracts: 2 passed, 0 failed, 0 signature_changed, 0 removed\nstderr:\n"
  m5 = m4.append(Message(role: "assistant", content: Json.encode(t5.action))).append(
    Message(role: "tool", content: obs5)
  )

  network = Network.fake(
    Http.replies([
      build_exchange(
        endpoint,
        key,
        TurnRequest(model: "sonnet", goal: goal, step: 1, messages: m0),
        t1
      ),
      build_exchange(
        endpoint,
        key,
        TurnRequest(model: "sonnet", goal: goal, step: 2, messages: m1),
        t2
      ),
      build_exchange(
        endpoint,
        key,
        TurnRequest(model: "sonnet", goal: goal, step: 3, messages: m2),
        t3
      ),
      build_exchange(
        endpoint,
        key,
        TurnRequest(model: "sonnet", goal: goal, step: 4, messages: m3),
        t4
      ),
      build_exchange(
        endpoint,
        key,
        TurnRequest(model: "sonnet", goal: goal, step: 5, messages: m4),
        t5
      ),
      build_exchange(
        endpoint,
        key,
        TurnRequest(model: "sonnet", goal: goal, step: 6, messages: m5),
        t6
      )
    ])
  )

  caps = confine_workspace(files, cmd, network, "repo", "gateway.internal:8080")
  sqlite = Sqlite.in_memory()
  assert sqlite.open("harness.db") is Ok(db)
  coord = Actor.start(TaskCoordinator)
  assert coord.set_limits(10, 50) is Ok(_)

  assert run_agent(caps, db, coord, endpoint, key, "run-001", "sonnet", goal) is Ok(report)
  assert report.completed
  assert report.steps == 6
  assert report.subagents.size == 1
  assert report.spent_usd == 0.01995
  assert files.read_text("repo/clamp.mo") == Ok(good_patch)

  assert load_turns(db, "run-001") is Ok(saved_turns)
  assert saved_turns.size == 6
  assert saved_turns.last is Some(last_turn)
  assert last_turn.spent_usd == 0.01995
end

test "hallucinated workspace escapes, disallowed binaries, and budget overruns are contained safely"
  files = Files.in_memory({"repo/app.mo": "module App\n", "secret.env": "PROD_KEY=xyz\n"})
  cmd = Command.fake({})
  network = Network.fake({})
  caps = confine_workspace(files, cmd, network, "repo", "gateway.internal:8080")
  coord = Actor.start(TaskCoordinator)

  # 1. File escape outside repo/ is blocked by Files.within("repo")
  read_escape = execute_tool(caps, coord, ReadFile(path: "../secret.env"))
  assert read_escape.starts_with?("file_error:")
  write_escape = execute_tool(caps, coord, WriteFile(path: "../pwn.txt", content: "x"))
  assert write_escape.starts_with?("file_error:")
  assert not files.exists?("pwn.txt")

  # 2. Disallowed program basename and path-traversal binary are blocked by Command.allow_programs(["mo", "git"])
  curl_attempt = execute_tool(caps, coord, RunProgram(program: "curl", args: ["https://evil.org"]))
  assert curl_attempt == "command_error: not_permitted curl"
  sh_attempt = execute_tool(caps, coord, RunProgram(program: "../bin/sh", args: ["-c", "id"]))
  assert sh_attempt == "command_error: not_permitted ../bin/sh"

  # 3. Network egress to any host outside gateway.internal:8080 is refused
  assert caps.network.connect("evil.org", 443, 1.second) is
    Error(Network.Refused(host: "evil.org", port: 443))

  # 4. Exact Decimal budget cap halts runaway token spend and records failure in SQLite
  endpoint = "http://gateway.internal:8080/v1/agent/turn"
  expensive = ModelTurn(
    thought: "Huge context turn.",
    input_tokens: 2_000_000,
    output_tokens: 500_000,
    action: Complete(summary: "done")
  )
  net_budget = Network.fake(
    Http.replies([
      build_exchange(
        endpoint,
        "k",
        TurnRequest(
          model: "opus",
          goal: "audit",
          step: 1,
          messages: [Message(role: "user", content: "audit")]
        ),
        expensive
      )
    ])
  )
  tight_caps = confine_workspace(files, cmd, net_budget, "repo", "gateway.internal:8080")
  tight_coord = Actor.start(TaskCoordinator)
  assert tight_coord.set_limits(5, 100) is Ok(_)
  sqlite = Sqlite.in_memory()
  assert sqlite.open("budget.db") is Ok(db)
  assert run_agent(tight_caps, db, tight_coord, endpoint, "k", "run-over", "opus", "audit") is
    Error(BudgetStopped(BudgetExceeded(spent_usd: 13.5, budget_usd: 1.0)))
  assert tight_coord.view.status ==
    Stopped(reason: "BudgetExceeded(spent_usd: 13.5, budget_usd: 1)")

  # 5. CLI usage status 2 on empty args, plus run + inspect end-to-end
  usage_console = Console.fake()
  assert main(
    Args.of([]),
    Env.of({}),
    usage_console,
    files,
    cmd,
    network,
    sqlite,
    Actor.start(TaskCoordinator)
  ) ==
    2

  cli_endpoint = "http://127.0.0.1:8080/v1/agent/turn"
  done_turn = ModelTurn(
    thought: "Workspace already clean.",
    input_tokens: 100,
    output_tokens: 20,
    action: Complete(summary: "All checks green.")
  )
  cli_net = Network.fake(
    Http.replies([
      build_exchange(
        cli_endpoint,
        "dev-key",
        TurnRequest(
          model: "claude-sonnet-4-6",
          goal: "check",
          step: 1,
          messages: [Message(role: "user", content: "check")]
        ),
        done_turn
      )
    ])
  )
  cli_sqlite = Sqlite.in_memory()
  cli_console = Console.fake()
  assert main(
    Args.of(["run", "repo", "cli.db", "cli-1", "check"]),
    Env.of({}),
    cli_console,
    files,
    cmd,
    cli_net,
    cli_sqlite,
    Actor.start(TaskCoordinator)
  ) ==
    0
  inspect_console = Console.fake()
  assert main(
    Args.of(["inspect", "cli.db", "cli-1"]),
    Env.of({}),
    inspect_console,
    files,
    cmd,
    cli_net,
    cli_sqlite,
    Actor.start(TaskCoordinator)
  ) ==
    0
  assert inspect_console.printed.size == 1
end
