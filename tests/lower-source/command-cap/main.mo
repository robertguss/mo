module Main
use Command

fn main() : Int
  spec = Command.spec("mo", ["check", "main.mo"]).in_folder("workspace")
  if spec.cwd == "workspace" and spec.program == "mo" and Command.ok_output("ok").ok?: 42 else: 1
end

test "lower-source command capability"
  cmd = Command.fake(
    Command.replies([(Command.spec("mo", ["check", "main.mo"]), Command.ok_output("clean\n"))])
  )
  assert cmd.allow_programs(["mo"]).run("mo", ["check", "main.mo"], 5.seconds) ==
    Ok(Command.Output(status: 0, stdout: "clean\n", stderr: ""))
end
