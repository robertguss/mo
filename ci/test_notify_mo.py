#!/usr/bin/env python3

import http.server
import io
import json
import os
import pathlib
import pty
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import unittest


ROOT = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = ROOT / ".claude/hooks/notify-mo.sh"
SETTINGS = ROOT / ".claude/settings.json"
BRANCH = "known-branch"
HEADER = 'X-Mo-Auth: Bearer s3cret "quoted"'
SCRUBBED = ("MO_WEBHOOK_URL", "MO_WEBHOOK_HEADER", "http_proxy", "all_proxy", "ALL_PROXY")


class Recorder(http.server.BaseHTTPRequestHandler):
    def record(self):
        length = int(self.headers.get("Content-Length", 0))
        self.server.requests.append({
            "method": self.command,
            "path": self.path,
            "headers": self.headers,
            "body": self.rfile.read(length),
        })
        self.send_response(204)
        self.end_headers()

    do_GET = do_POST = do_PUT = record

    def log_message(self, format, *args):
        pass


class NotifyMo(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="mo-notify-")
        self.addCleanup(temporary.cleanup)
        base = pathlib.Path(os.path.realpath(temporary.name))
        self.home = base / "home"
        self.repo = base / "repo"
        self.plain = base / "plain"
        for folder in (self.home, self.repo, self.plain):
            folder.mkdir()
        self.environment = {name: value for name, value in os.environ.items()
                            if name not in SCRUBBED}
        self.environment["HOME"] = str(self.home)
        subprocess.run(["git", "init", "--quiet", "--initial-branch", BRANCH, str(self.repo)],
                       env=self.environment, check=True)

        self.server = http.server.HTTPServer(("127.0.0.1", 0), Recorder)
        self.server.requests = []
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}/hooks/mo"
        self.hook_input = json.dumps({"cwd": str(self.repo), "session_id": "abc"}).encode()

    def write_env_text(self, text):
        env_file = self.home / ".config/mo/webhook.env"
        env_file.parent.mkdir(parents=True, exist_ok=True)
        env_file.write_text(text)

    def write_env(self, **variables):
        self.write_env_text("".join(f"{name}={shlex.quote(value)}\n"
                                    for name, value in variables.items()))

    def run_hook(self, *arguments, command=None, extra_environment=None, timeout=15, **stdin):
        result = subprocess.run(command or [str(SCRIPT), *arguments], cwd=self.repo,
                                env={**self.environment, **(extra_environment or {})},
                                capture_output=True, timeout=timeout, **stdin)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr, b"")

    def body(self, cwd, branch, session_id, **extra):
        return {
            "cwd": str(cwd),
            "branch": branch,
            "session_id": session_id,
            "repo": "robertguss/mo",
            "event": "stop",
            **extra,
        }

    def assert_one_ping(self, expected, path="/hooks/mo"):
        self.assertEqual(len(self.server.requests), 1)
        request = self.server.requests[0]
        self.assertEqual(request["method"], "POST")
        self.assertEqual(request["path"], path)
        self.assertEqual(request["headers"].get_all("X-Mo-Auth"), ['Bearer s3cret "quoted"'])
        self.assertEqual(request["headers"].get_all("Content-Type"), ["application/json"])
        self.assertEqual(json.loads(request["body"]), expected)

    def test_no_env_file_stays_silent(self):
        self.run_hook(input=self.hook_input)
        self.assertEqual(self.server.requests, [])

    def test_env_file_missing_a_variable_stays_silent(self):
        complete = {"MO_WEBHOOK_URL": self.url, "MO_WEBHOOK_HEADER": HEADER}
        for missing in complete:
            with self.subTest(missing=missing):
                self.write_env(**{name: value for name, value in complete.items()
                                  if name != missing})
                self.run_hook(input=self.hook_input)
                self.assertEqual(self.server.requests, [])

    def test_exported_variables_without_an_env_file_stay_silent(self):
        self.run_hook(input=self.hook_input,
                      extra_environment={"MO_WEBHOOK_URL": self.url, "MO_WEBHOOK_HEADER": HEADER})
        self.assertEqual(self.server.requests, [])

    def test_env_file_that_exits_nonzero_exits_zero(self):
        self.write_env_text("exit 2\n")
        self.run_hook(input=self.hook_input)
        self.assertEqual(self.server.requests, [])

    def test_env_file_with_errors_stays_silent(self):
        self.write_env_text("echo out\necho err >&2\nif\n")
        self.run_hook(input=self.hook_input)
        self.assertEqual(self.server.requests, [])

    def test_hook_stdin_posts_the_ping(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        self.run_hook(input=self.hook_input)
        self.assert_one_ping(self.body(self.repo, BRANCH, "abc"))

    def test_test_flag_marks_the_ping(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        self.run_hook("--test", input=self.hook_input)
        self.assert_one_ping(self.body(self.repo, BRANCH, "", test=True))

    def test_test_flag_at_a_terminal_does_not_hang(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        controller, terminal = pty.openpty()
        self.addCleanup(os.close, controller)
        self.addCleanup(os.close, terminal)
        self.run_hook("--test", stdin=terminal)
        self.assert_one_ping(self.body(self.repo, BRANCH, "", test=True))

    def test_test_flag_with_a_pipe_left_open_does_not_hang(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        reader, writer = os.pipe()
        self.addCleanup(os.close, reader)
        self.addCleanup(os.close, writer)
        self.run_hook("--test", stdin=reader)
        self.assert_one_ping(self.body(self.repo, BRANCH, "", test=True))

    def test_malformed_stdin_still_posts(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        self.run_hook(input=b"{not json")
        self.assert_one_ping(self.body(self.repo, BRANCH, ""))

    def test_stdin_that_is_not_an_object_stays_silent(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        self.run_hook(input=b"[]")
        self.assertEqual(self.server.requests, [])

    def test_cwd_outside_a_git_repo_posts_an_empty_branch(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        self.run_hook(input=json.dumps({"cwd": str(self.plain), "session_id": "abc"}).encode())
        self.assert_one_ping(self.body(self.plain, "", "abc"))

    def test_connection_refused_stays_silent(self):
        with socket.create_server(("127.0.0.1", 0)) as closed:
            port = closed.getsockname()[1]
        self.write_env(MO_WEBHOOK_URL=f"http://127.0.0.1:{port}/hooks/mo",
                       MO_WEBHOOK_HEADER=HEADER)
        self.run_hook(input=self.hook_input)

    def test_server_that_never_answers_stays_silent_and_bounded(self):
        with socket.create_server(("127.0.0.1", 0)) as silent:
            port = silent.getsockname()[1]
            self.write_env(MO_WEBHOOK_URL=f"http://127.0.0.1:{port}/hooks/mo",
                           MO_WEBHOOK_HEADER=HEADER)
            self.run_hook(input=self.hook_input, timeout=8)

    def test_url_with_a_quote_and_a_backslash_arrives_intact(self):
        self.write_env(MO_WEBHOOK_URL=f'{self.url}/a"b\\c', MO_WEBHOOK_HEADER=HEADER)
        self.run_hook(input=self.hook_input)
        self.assert_one_ping(self.body(self.repo, BRANCH, "abc"), path='/hooks/mo/a"b\\c')

    def test_url_is_not_globbed(self):
        self.write_env(MO_WEBHOOK_URL=f"{self.url}/{{a,b}}", MO_WEBHOOK_HEADER=HEADER)
        self.run_hook(input=self.hook_input)
        self.assert_one_ping(self.body(self.repo, BRANCH, "abc"), path="/hooks/mo/{a,b}")

    def test_xtrace_does_not_print_the_secrets(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        result = subprocess.run(["bash", "-x", str(SCRIPT)], cwd=self.repo, env=self.environment,
                                input=self.hook_input, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0)
        self.assertNotIn(b"s3cret", result.stderr)
        self.assertNotIn(self.url.encode(), result.stderr)
        self.assert_one_ping(self.body(self.repo, BRANCH, "abc"))

    def test_curl_gets_the_time_limit_and_neither_secret(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        shim = self.plain / "curl"
        arguments = self.plain / "arguments"
        shim.write_text('#!/bin/sh\nprintf \'%s\\n\' "$@" >"$CURL_ARGUMENTS"\n')
        shim.chmod(0o755)
        self.run_hook(input=self.hook_input, extra_environment={
            "PATH": f"{self.plain}:{os.environ['PATH']}",
            "CURL_ARGUMENTS": str(arguments),
        })
        recorded = arguments.read_text()
        self.assertIn("--max-time\n5\n", recorded)
        self.assertNotIn("s3cret", recorded)
        self.assertNotIn(self.url, recorded)

    def test_curlrc_is_ignored(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        (self.home / ".curlrc").write_text('header = "X-From-Curlrc: yes"\n')
        self.run_hook(input=self.hook_input)
        self.assert_one_ping(self.body(self.repo, BRANCH, "abc"))
        self.assertIsNone(self.server.requests[0]["headers"].get("X-From-Curlrc"))

    def test_python_files_in_the_session_folder_are_ignored(self):
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        (self.repo / "json.py").write_text("raise SystemExit(3)\n")
        self.run_hook(input=self.hook_input)
        self.assert_one_ping(self.body(self.repo, BRANCH, "abc"))

    def test_settings_entry_runs_the_hook(self):
        settings = json.loads(SETTINGS.read_text())
        self.assertEqual(settings["enabledPlugins"],
                         {"compound-engineering@compound-engineering-plugin": False})
        command = settings["hooks"]["Stop"][0]["hooks"][0]["command"]
        self.write_env(MO_WEBHOOK_URL=self.url, MO_WEBHOOK_HEADER=HEADER)
        self.run_hook(command=["bash", "-c", command],
                      extra_environment={"CLAUDE_PROJECT_DIR": str(ROOT)},
                      input=self.hook_input)
        self.assert_one_ping(self.body(self.repo, BRANCH, "abc"))


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(NotifyMo)
    stream = io.StringIO()
    result = unittest.TextTestRunner(stream=stream).run(suite)
    if not result.wasSuccessful():
        sys.exit(stream.getvalue())
    print(f"Claude Stop hook: {result.testsRun} cases passed")
