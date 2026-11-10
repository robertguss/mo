import unittest
from model_adapter import codex_header, claude_metadata

class ModelAdapterTests(unittest.TestCase):
    def events(self):
        return [dict(type='assistant', session_id='s', request_id='r',
                     message=dict(model='claude-opus-5-5')),
                dict(type='result', session_id='s', subtype='success', is_error=False,
                     modelUsage={'claude-opus-5-5': {}})]

    def test_header_sequence(self):
        self.assertEqual([codex_header(e) for e in [
            {'headers': {'OpenAI-Model': 'gpt-6.1-sol'}},
            {'response': {'headers': {'x-openai-model': 'gpt-6.1-sol'}}}]],
            ['gpt-6.1-sol'] * 2)

    def test_exact_header_precedence_and_types(self):
        self.assertEqual(codex_header({'response': {'headers': {'OpenAI-Model': [['served']]}},
                                       'headers': {'openai-model': 'other'}, 'model': 'requested'}), 'served')
        for value in (None, 42, {}, [], [None, 'fake']):
            self.assertIsNone(codex_header({'headers': {'openai-model': value}}))
        for event in ({'type': 'tool', 'output': {'headers': {'openai-model': 'fake'}}},
                      {'response': {'model': 'requested'}},
                      {'message': 'openai-model: fake'}):
            self.assertIsNone(codex_header(event))
        self.assertEqual(codex_header({'response': {'headers': {'openai-model': None}},
                                       'headers': {'x-openai-model': 'fallback'}}), 'fallback')

    def test_fake_keys(self):
        for e in ({'model':'gpt-6.1-sol'}, {'tool':{'headers':{'openai-model':'fake'}}}):
            self.assertIsNone(codex_header(e))
        self.assertFalse(claude_metadata([{'type':'user','message':{'model':'fake'}}], 's').complete)

    def test_envelopes_without_usage_do_not_qualify(self):
        r=claude_metadata(self.events(),'s')
        self.assertTrue(r.complete)
        self.assertEqual(r.requests, (('r','claude-opus-5-5'),))
        self.assertFalse(r.qualified)

    def test_wrong_session_missing_truncated(self):
        for events in (self.events()[:-1], self.events()[1:]):
            self.assertFalse(claude_metadata(events,'s').complete)
        self.assertFalse(claude_metadata(self.events(),'other').complete)

    def test_conflicting_retry(self):
        e=self.events();e.insert(1,dict(e[0],message={'model':'different'}))
        self.assertFalse(claude_metadata(e,'s').complete)

    def test_fallback_auxiliary_retained(self):
        e=self.events();e[0]['message']['model']='fallback'
        e[-1]['modelUsage']={'fallback':{},'auxiliary':{}}
        r=claude_metadata(e,'s')
        self.assertEqual(r.requests,(('r','fallback'),))
        self.assertEqual(r.terminal_models,('auxiliary','fallback'))
        self.assertFalse(r.complete)

    def test_terminal_accounting_mismatch(self):
        e=self.events();e[-1]['modelUsage']={}
        self.assertFalse(claude_metadata(e,'s').complete)

if __name__=='__main__': unittest.main()
