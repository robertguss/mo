## Confined subprocess execution (`Command`): explicit working-folder and program-basename
## attenuation, bounded output and deadlines, and deterministic `Command.fake` exchanges for tests.
module Command
expose Output(..), Spec(..), CommandError(..), spec, in_folder, output, ok_output, ok?, replies, run

struct Output
  status: Int
  stdout: Text
  stderr: Text
end

struct Spec
  cwd: Text
  program: Text
  args: List(Text)
end

enum CommandError
  OutsideFolder(path: Text)
  NotPermitted(program: Text)
  TimedOut
  OutputTooLarge
  SpawnFailed(reason: Text)
end

## A command specification at the root folder (`""`) for `Command.replies`.
fn spec(program: Text, args: List(Text)) : Spec
  examples
    spec("mo", ["test", "main.mo"]) == Spec(cwd: "", program: "mo", args: ["test", "main.mo"])
  end

  Spec(cwd: "", program: program, args: args)
end

fn normalize_cwd(folder: Text) : Text
  trimmed = folder.drop_prefix("./") or folder
  if trimmed == ".": "" else: trimmed
end

## Narrows a `Spec` to run inside a relative subfolder (matching `cmd.within(folder)`).
fn in_folder(s: Spec, folder: Text) : Spec
  examples
    spec("git", ["status"]).in_folder("repo") == Spec(cwd: "repo", program: "git", args: ["status"])
    spec("mo", ["check", "main.mo"]).in_folder(".") ==
      Spec(cwd: "", program: "mo", args: ["check", "main.mo"])
  end

  rel = normalize_cwd(folder)
  next_cwd = if s.cwd.empty?
    rel
  else if rel.empty?
    s.cwd
  else
    "#{s.cwd}/#{rel}"
  end
  Spec(cwd: next_cwd, program: s.program, args: s.args)
end

## Builds an `Output` with exit status, stdout, and stderr.
fn output(status: Int, stdout: Text, stderr: Text) : Output
  examples
    output(1, "", "error\n").status == 1
  end

  Output(status: status, stdout: stdout, stderr: stderr)
end

## Builds a zero-exit `Output` with `stdout` and empty `stderr`.
fn ok_output(stdout: Text) : Output
  examples
    ok_output("ok\n") == Output(status: 0, stdout: "ok\n", stderr: "")
  end

  Output(status: 0, stdout: stdout, stderr: "")
end

## True when the command exited with status 0.
fn ok?(out: Output) : Bool
  examples
    ok_output("pass").ok?
    not output(1, "", "fail").ok?
  end

  out.status == 0
end

fn encode_spec(s: Spec) : Bytes
  var parts = [
    "#{s.cwd.byte_count}:#{s.cwd}\n".bytes,
    "#{s.program.byte_count}:#{s.program}\n".bytes,
    "#{s.args.size}\n".bytes
  ]
  for arg in s.args
    parts = parts.append("#{arg.byte_count}:#{arg}\n".bytes)
  end
  Bytes.join(parts)
end

fn encode_output(out: Output) : Bytes
  out_b = out.stdout.bytes
  err_b = out.stderr.bytes
  Bytes.join(["#{out.status}:#{out_b.size}:#{err_b.size}\n".bytes, out_b, err_b])
end

## Encodes expected `(Spec, Output)` exchanges into the `Map(Bytes, Bytes)` table `Command.fake`
## reads in tests, examples, and promises.
fn replies(exchanges: List((Spec, Output))) : Map(Bytes, Bytes)
  examples
    replies([(spec("mo", ["check", "main.mo"]), ok_output(""))]).size == 1
  end

  var table = {}
  for (s, out) in exchanges
    table = table.set(encode_spec(s), encode_output(out))
  end
  table
end

fn parse_error(raw: Text) : CommandError
  if raw.starts_with?("outside_folder:")
    OutsideFolder(path: raw.drop_prefix("outside_folder:") or raw)
  else if raw.starts_with?("not_permitted:")
    NotPermitted(program: raw.drop_prefix("not_permitted:") or raw)
  else if raw == "timed_out"
    TimedOut
  else if raw == "output_too_large"
    OutputTooLarge
  else if raw.starts_with?("spawn_failed:")
    SpawnFailed(reason: raw.drop_prefix("spawn_failed:") or raw)
  else
    SpawnFailed(reason: raw)
  end
end

## Runs `program` with `args` inside `command`'s confined folder, returning `Output` or a
## structured `CommandError` if containment, program allowlist, UTF-8, output limit, or `within`
## deadline refuses it.
fn run(command: Command, program: Text, args: List(Text), within: Duration) : Result(
  Output,
  CommandError
)
  examples
    Command.fake(replies([(spec("mo", ["test", "main.mo"]), ok_output("1 passed\n"))])).run(
      "mo",
      ["test", "main.mo"],
      5.seconds
    ) ==
      Ok(Output(status: 0, stdout: "1 passed\n", stderr: ""))
  end

  case command.run_raw(program, args, within)
    Error(raw): Error(parse_error(raw))
    Ok((status, out_b, err_b)):
      case (out_b.decode_utf8, err_b.decode_utf8)
        (Some(stdout), Some(stderr)): Ok(Output(status: status, stdout: stdout, stderr: stderr))
        _: Error(SpawnFailed(reason: "command output is not valid UTF-8"))
      end
  end
end

test "fake command enforces folder containment and monotonic program allowlists"
  cmd = Command.fake(
    replies([
      (spec("mo", ["test", "main.mo"]).in_folder("workspace"), ok_output("2 passed\n")),
      (spec("git", ["status", "-s"]).in_folder("workspace"), output(0, " M main.mo\n", "")),
      (spec("curl", ["https://evil.example"]).in_folder("workspace"), ok_output("nope"))
    ])
  )
  scoped = cmd.within("workspace").allow_programs(["mo", "git"])
  assert scoped.run("mo", ["test", "main.mo"], 10.seconds) ==
    Ok(Output(status: 0, stdout: "2 passed\n", stderr: ""))
  assert scoped.run("git", ["status", "-s"], 5.seconds) ==
    Ok(Output(status: 0, stdout: " M main.mo\n", stderr: ""))
  assert scoped.run("curl", ["https://evil.example"], 5.seconds) ==
    Error(NotPermitted(program: "curl"))
  assert scoped.run("../sh", ["-c", "id"], 5.seconds) == Error(NotPermitted(program: "../sh"))

  narrower = scoped.allow_programs(["mo", "curl"])
  assert narrower.run("git", ["status", "-s"], 5.seconds) == Error(NotPermitted(program: "git"))

  escaped = cmd.within("../etc")
  assert escaped.run("mo", ["check"], 5.seconds) == Error(OutsideFolder(path: "../etc"))
end
