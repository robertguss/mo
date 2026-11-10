import json
import os
import unittest
from pathlib import Path

from model_adapter import claude_metadata

HERE = Path(__file__).resolve().parent


def usage(i, read, made):
    return {'input_tokens': i, 'cache_read_input_tokens': read, 'cache_creation_input_tokens': made,
            'output_tokens': 1}


def envelope(rid, use, model='claude-opus-5-5', mid=None):
    return dict(type='assistant', session_id='s', request_id=rid,
                message=dict(id=mid or 'm' + rid, model=model, usage=use))


def result(models):
    return dict(type='result', session_id='s', subtype='success', is_error=False,
                modelUsage={m: {'inputTokens': i, 'cacheReadInputTokens': r, 'cacheCreationInputTokens': c,
                                'outputTokens': 9} for m, (i, r, c) in models.items()})


class ClaudeProvenanceTests(unittest.TestCase):
    def good(self):
        return [dict(type='system', session_id='s'),
                envelope('r1', usage(2, 100, 50)), envelope('r1', usage(2, 100, 50)),
                envelope('r2', usage(3, 200, 10)),
                result({'claude-opus-5-5': (5, 300, 60)})]

    def test_reconciled_envelopes_qualify(self):
        r = claude_metadata(self.good(), 's')
        self.assertTrue(r.qualified, r)
        self.assertEqual(r.requests, (('r1', 'claude-opus-5-5'), ('r2', 'claude-opus-5-5')))

    def test_unenumerated_request_fails(self):
        e = self.good()
        e[-1] = result({'claude-opus-5-5': (6, 300, 60)})
        self.assertFalse(claude_metadata(e, 's').qualified)
        del e[3]
        e[-1] = result({'claude-opus-5-5': (5, 300, 60)})
        self.assertFalse(claude_metadata(e, 's').qualified)

    def test_auxiliary_or_fallback_model_is_retained_and_fails(self):
        e = self.good()
        e[-1] = result({'claude-opus-5-5': (5, 300, 60), 'claude-haiku-5-5': (1, 0, 0)})
        r = claude_metadata(e, 's')
        self.assertFalse(r.qualified)
        self.assertIn('claude-haiku-5-5', r.terminal_models)

    def test_inconsistent_or_missing_usage_fails(self):
        e = self.good()
        e[2] = envelope('r1', usage(2, 101, 50))
        self.assertFalse(claude_metadata(e, 's').qualified)
        e = self.good()
        for i in (1, 2):
            e[i]['message'].pop('usage')
        self.assertFalse(claude_metadata(e, 's').qualified)
        e = self.good()
        e[2] = envelope('r1', usage(2, 100, 50), mid='other')
        self.assertFalse(claude_metadata(e, 's').qualified)

    def test_original_scored_claude_streams_reconcile(self):
        for slot in (2, 4):
            path = HERE.parent / f'results/experiment-2-original/slot-{slot}/stream.jsonl'
            events = [json.loads(l) for l in path.read_text().splitlines() if l.strip()]
            session = next(e['session_id'] for e in events if e.get('type') == 'system')
            r = claude_metadata(events, session)
            self.assertTrue(r.qualified, slot)
            self.assertGreater(len(r.requests), 20)

    @unittest.skipUnless(os.access(Path.home() / '.mo-phase5-harness', os.R_OK), 'private capture absent')
    def test_retained_development_stream_reconciles(self):
        path = Path.home() / '.mo-phase5-harness/u1-live-20261109/claude.jsonl'
        events = [json.loads(l) for l in path.read_text().splitlines() if l.strip()]
        session = next(e['session_id'] for e in events if e.get('type') == 'system')
        self.assertTrue(claude_metadata(events, session).qualified)


if __name__ == '__main__':
    unittest.main()
