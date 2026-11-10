"""Development-only negative controls. Failures block U2 readiness.

Run directly; these intentionally expose the existing untrusted origin matcher.
No model, service, credentials, firewall rules, or agent processes are needed.
"""
import json
import pathlib
import tempfile
import unittest
import proxy


class OriginContract(unittest.TestCase):
    def check_untrusted(self, commands, count=1, answered=True, forged=False):
        with tempfile.TemporaryDirectory() as tmp:
            stream, log = (pathlib.Path(tmp) / name for name in ('stream', 'log'))
            stream.write_text(''.join(json.dumps({'type': 'item.completed', 'item': {
                'id': str(i), 'type': 'command_execution', 'command': command,
                'exit_code': 0}}) + '\n' for i, command in enumerate(commands)))
            rows = []
            for i in range(count):
                request = {'seq': 2*i+1, 'direction': 'request', 'rpc': 'tools/call',
                           'id': 1, 'tool': 'mo-upgrade-open',
                           'body': {'params': {'arguments': {}}}}
                if forged:
                    request['origin'] = {'command': 'mo push', 'pid': 1}
                rows.append(request)
                if answered:
                    rows.append({'seq': 2*i+2, 'direction': 'response', 'request': 2*i+1})
            log.write_text(''.join(json.dumps(row) + '\n' for row in rows))
            result = proxy.match('codex', stream, log)
            self.assertFalse(result['match'], result)
            self.assertEqual(result['command_origin'], [], result)

    def test_push_text_without_process_evidence(self):
        self.check_untrusted(['mo push main.mo'])

    def test_repeated_rpc_ids_duplicate_arguments(self):
        self.check_untrusted(['mo push main.mo'], count=2)

    def test_unrelated_http_beside_push(self):
        self.check_untrusted(['mo push main.mo & curl localhost'], count=2)

    def test_forged_origin_is_not_evidence(self):
        self.check_untrusted(['mo push main.mo'], forged=True)

    def test_echo_is_not_execution(self):
        self.check_untrusted(["echo 'mo push main.mo'"])

    def test_unanswered_is_not_attested(self):
        self.check_untrusted(['mo push main.mo'], answered=False)


if __name__ == '__main__':
    unittest.main()
