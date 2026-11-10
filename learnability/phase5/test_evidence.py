"""Development tests; no service drill or agent session is collected here."""
import unittest
from evidence import evaluation_gate, build_inputs


class EvidenceTests(unittest.TestCase):
    def test_missing_and_duplicate_slots_cannot_pass(self):
        rows = [{'slot': n, 'client': c, 'correct': True}
                for n, c in enumerate(['codex', 'claude', 'codex', 'claude'], 1)]
        self.assertTrue(evaluation_gate(rows, True)['passed'])
        self.assertFalse(evaluation_gate(rows[:-1], True)['passed'])
        self.assertFalse(evaluation_gate(rows + rows[:1], True)['passed'])
        self.assertFalse(evaluation_gate(rows, False)['passed'])

    def test_one_miss_permitted_but_not_a_missing_client(self):
        rows = [{'slot': n, 'client': c, 'correct': n != 1}
                for n, c in enumerate(['codex', 'claude', 'codex', 'claude'], 1)]
        self.assertTrue(evaluation_gate(rows, True)['passed'])
        rows[2]['correct'] = False
        self.assertFalse(evaluation_gate(rows, True)['passed'])

    def test_development_never_counts(self):
        rows = [{'slot': n, 'client': c, 'correct': True, 'development': True}
                for n, c in enumerate(['codex', 'claude', 'codex', 'claude'], 1)]
        self.assertFalse(evaluation_gate(rows, True)['passed'])


if __name__ == '__main__':
    unittest.main()
