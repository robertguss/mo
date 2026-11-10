import copy
import unittest
from origin_join import join, JoinError, digests
from origin_trace import parse_trace
from test_origin_trace import line


class JoinTests(unittest.TestCase):
    def fixture(self):
        trace = parse_trace(line(b'POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}') + '\n' +
                            line(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}', op='read'))
        metadata = dict(endpoint='127.0.0.1:4000->127.0.0.1:80', request_ordinal=1,
                        **digests(b'{}'))
        rows = [dict(seq=1, direction='request', connection=1, http='POST', transport=metadata),
                dict(seq=2, direction='response', request=1, status=200, transport=digests(b'{}'))]
        return trace, rows

    def test_positive(self):
        result = join(*self.fixture())
        self.assertEqual(len(result['observations']), 1)
        self.assertFalse(result['qualified'])
        self.assertEqual(result['command_origin'], [])

    def test_missing_forged_duplicate_replay(self):
        for mutation in range(6):
            trace, rows = self.fixture()
            if mutation == 0: rows[0].pop('transport')
            if mutation == 1: rows[0]['transport']['body_sha256'] = 'forged'
            if mutation == 2: rows.append(copy.deepcopy(rows[0]))
            if mutation == 3: trace['requests'].append(copy.deepcopy(trace['requests'][0]))
            if mutation == 4: trace['responses'] = []
            if mutation == 5: rows[1]['status'] = 500
            with self.assertRaises(JoinError): join(trace, rows)

    def test_request_unanswered_on_both_sides_stays_explicit(self):
        trace, rows = self.fixture()
        trace['responses'] = []
        result = join(trace, rows[:1])
        self.assertEqual(result['unanswered'], [1])
        self.assertEqual(len(result['observations']), 1)
        with self.assertRaises(JoinError): join(trace, rows)
        trace, rows = self.fixture()
        with self.assertRaises(JoinError): join(trace, rows[:1])
        trace, rows = self.fixture()
        trace['responses'] = []
        rows[0]['transport']['body_sha256'] = 'forged'
        with self.assertRaises(JoinError): join(trace, rows[:1])
