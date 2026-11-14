import http.client
import json
import locale
import pathlib
import select
import shutil
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parent.parent

# Select an installed UTF-8 locale for the raw-text regression, independently
# of the CI shell's locale. C/POSIX/unavailable locales are tested separately.
for utf8_locale in ("C.UTF-8", "en_US.UTF-8", "UTF-8"):
    try:
        locale.setlocale(locale.LC_CTYPE, utf8_locale)
        break
    except locale.Error:
        pass
else:
    raise AssertionError("CLI display tests need an installed UTF-8 locale")


def run(binary, args, status, env=None):
    result = subprocess.run([str(binary), *args], cwd=root, env=env, capture_output=True, timeout=30)
    assert result.returncode == status, (args, result.returncode, result.stderr)
    return result


def records(result, command):
    assert not result.stderr, result.stderr
    lines = result.stdout.decode("utf-8").splitlines()
    assert result.stdout.endswith(b"\n")
    items = [json.loads(line) for line in lines]
    assert items[0] == {"record": "version", "format": "1.0", "mo": "0.2.0", "command": command}
    assert items[-1]["record"] == "totals"
    assert items[-1]["status"] == result.returncode
    assert items[-1]["errors"] == sum(item["record"] == "diagnostic" for item in items)
    return items


SERVE_COUNTER = """module Counter

service
  state Int = 0
  tools add, read
  token from env "COUNTER_TOKEN"
end

## Adds an amount to the count.
fn add(state: Int, _now: Time, amount: Int) : (Int, Int)
  (state + amount, state + amount)
end

## Reads the count.
fn read(state: Int, _now: Time) : Text
  state.to_text
end
"""
SERVE_HALTING = (SERVE_COUNTER.replace("tools add, read", "tools add, read, halt") +
                 '\n## Always stops.\nfn halt(_state: Int, _now: Time) : Text\n'
                 '  stop "halted on purpose"\nend\n')
SERVE_EDITS = (("(state + amount, state + amount)", "(state + amount, state * amount)"),
               ("state Int = 0", "state Int = 1"),
               ("## Reads the count.", "## Reads the total."),
               ("## Reads the count.", "\n## Reads the count."))
SERVE_TOOL_PAGE = 32
SERVE_OPERATOR_NAMES = ["mo-overview", "mo-state", "mo-mailbox", "mo-timers", "mo-memory", "mo-supervisor", "mo-log", "mo-export", "mo-failures"]
# Listed after the views; they need the upgrade credential and change the service.
SERVE_UPGRADE_NAMES = ["mo-upgrade-open", "mo-upgrade-page", "mo-upgrade-commit"]
SERVE_REVIEW_NAMES = ["mo-intent-items", "mo-intent-approve", "mo-review-setting"]
SERVE_OVERLOADED = """module Overloaded

struct Count
  value: Int
end

struct Spare
  value: Int
end

service
  state Count = Count(value: 0)
  tools halt
  every 1.hour tick
  token from env "COUNTER_TOKEN"
end

## Always stops.
fn halt(_state: Count, _now: Time) : Text
  stop "tool failed"
end

fn halt(spare: Spare) : Int
  spare.value
end

fn tick(_state: Count, _now: Time) : Count
  stop "tick failed"
end

fn tick(spare: Spare) : Int
  spare.value
end
"""
SERVE_FORGED = "mo serve: failure: forged: y"
SERVE_FORGING = f"""module Forging

service
  state Text = "x\\n{SERVE_FORGED}"
  tools read
  every 1.millisecond tick
  token from env "COUNTER_TOKEN"
end

## Reads the text.
fn read(state: Text, _now: Time) : List(Text)
  [state]
end

fn tick(_state: Text, _now: Time) : Text
  stop "x\\n{SERVE_FORGED}"
end
"""
SERVE_SIGNALLED = b"mo serve: stop: a signal asked the service to stop\n"
SERVE_INITIALIZE = ('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{'
                    '"protocolVersion":"2025-11-25","capabilities":{},'
                    '"clientInfo":{"name":"demo","version":"1"}}}')
SERVE_INITIALIZED = '{"jsonrpc":"2.0","method":"notifications/initialized"}'
SERVE_LIST = '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
SERVE_ADD = ('{"jsonrpc":"2.0","id":3,"method":"tools/call",'
             '"params":{"name":"add","arguments":{"amount":"5"}}}')
SERVE_READ = '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"read"}}'
SERVE_HALT = '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"halt"}}'


class Served:
    def __init__(self, binary, source, data, env, folder, port="0", extra=()):
        self.stderr = tempfile.NamedTemporaryFile(dir=folder, prefix="stderr-", delete=False)
        self.outputs = pathlib.Path(self.stderr.name)
        self.process = subprocess.Popen(
            [str(binary), "serve", str(source), "--port", port, "--data", str(data), *extra],
            env=env, stdout=subprocess.PIPE, stderr=self.stderr, cwd=folder, bufsize=0,
            umask=0o022)
        try:
            line = self.announcement()
        except BaseException:
            self.process.kill()
            self.process.wait()
            self.stderr.close()
            raise
        self.line = line
        self.port = None
        if line:
            prefix, suffix = b"mo serve: listening on http://127.0.0.1:", b"/mcp\n"
            assert line.startswith(prefix) and line.endswith(suffix), line
            self.port = int(line[len(prefix):-len(suffix)])

    def announcement(self):
        line = b""
        limit = time.monotonic() + 30
        while not line.endswith(b"\n"):
            ready = select.select([self.process.stdout], [], [], limit - time.monotonic())[0]
            assert ready, ("mo serve did not announce its port in time", line)
            chunk = self.process.stdout.read(1)
            if not chunk:
                break
            line += chunk
        return line

    def post(self, body, token, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        sent = {"Authorization": f"Bearer {token}", "Content-Type": "application/json",
                "MCP-Protocol-Version": "2025-11-25"}
        connection.request("POST", "/mcp", body, {**sent, **(headers or {})})
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response.status, data

    def finish(self, status, stop=None):
        if stop is not None:
            self.process.send_signal(stop)
        assert self.process.wait(timeout=30) == status, (self.process.returncode, self.errors())
        rest = self.process.stdout.read()
        assert not rest, rest
        return self.errors()

    def errors(self):
        return self.outputs.read_bytes()

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=30)
        self.process.stdout.close()
        self.stderr.close()


STORE_FAILED = b"service store failed; reopen it to see whether the last change was saved"
STORE_BUSY = (b"another process holds the service store, such as a second mo serve or a reader "
              b"of its data folder; stopped waiting")
SAVED_REFUSED = (b"saved service data is damaged or was written by a different program; "
                 b"recovery refused")


def serve_events(errors):
    return [line for line in errors.split(b"\n") if line.startswith(b"mo serve: ")]


def serve_supervised(name):
    reason = b"service handler failed"
    lines = []
    for restart in (1, 2, 3):
        lines += [b"mo serve: failure: %s: %s" % (name, reason)] * 5
        lines.append(b"mo serve: restart: %d of 3: %s" % (restart, reason))
    return lines + [b"mo serve: stop: " + reason]


def serve_structured(reply):
    status, body = reply
    assert status == 200, reply
    return json.loads(body)["result"]["structuredContent"]


def serve_spellings(folder, counter):
    for name in ("sub", "linked", "copied"):
        (folder / name).mkdir()
    (folder / "linked" / counter).symlink_to(folder / counter)
    (folder / "folder-link").symlink_to(folder, target_is_directory=True)
    shutil.copy(folder / counter, folder / "copied")
    return ("./counter.mo", folder / counter, "sub/../counter.mo", "linked/counter.mo",
            "folder-link/counter.mo", "copied/counter.mo", folder / "copied" / counter)
