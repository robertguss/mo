module Main
use Command

fn escapes?(cmd: Command, name: Text) : Bool
  promise "a generated command capability rejects escaping folders and disallowed programs"
    not result
    allows cmd: Command.fake({}), name: "mo", result: false
    forbids cmd: Command.fake({}), name: "mo", result: true
  end

  escaped = cmd.within("../sub")
  disallowed = cmd.allow_programs(["mo"])
  (escaped.run("mo", ["check"], 1.second) is Ok(_)) or (
    disallowed.run("curl", ["http://x"], 1.second) is Ok(_)
  ) or (cmd.run("../#{name}", [], 1.second) is Ok(_))
end

test "a fake command capability enforces folder narrowing and monotonic program allowlists"
  cmd = Command.fake(
    Command.replies([
      (Command.spec("mo", ["test", "app.mo"]).in_folder("work"), Command.ok_output("1 passed\n")),
      (
        Command.spec("git", ["diff", "--stat"]).in_folder("work"),
        Command.output(0, " 1 file\n", "")
      )
    ])
  )
  scoped = cmd.within("work").allow_programs(["mo", "git"])
  assert scoped.run("mo", ["test", "app.mo"], 5.seconds) ==
    Ok(Command.Output(status: 0, stdout: "1 passed\n", stderr: ""))
  assert scoped.run("git", ["diff", "--stat"], 5.seconds) ==
    Ok(Command.Output(status: 0, stdout: " 1 file\n", stderr: ""))
  assert scoped.allow_programs(["mo"]).run("git", ["diff", "--stat"], 5.seconds) ==
    Error(Command.NotPermitted(program: "git"))
  assert scoped.run("sh", ["-c", "id"], 5.seconds) == Error(Command.NotPermitted(program: "sh"))
  assert cmd.within("../outside").run("mo", ["check"], 5.seconds) ==
    Error(Command.OutsideFolder(path: "../outside"))
end
