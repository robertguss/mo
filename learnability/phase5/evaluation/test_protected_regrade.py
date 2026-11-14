from pathlib import Path
import unittest
from unittest.mock import patch
import protected_regrade as replay


class MissingActivation(unittest.TestCase):
    def test_absent_report_returns_identity_miss_without_compiling(self):
        with patch.object(replay.subprocess, 'run') as run:
            for report in (None, {'report': {'migration': 'invalid'}}):
                identity, reasons = replay.compile_identity('/unused/mo', Path('/unused'), report)
                self.assertEqual(identity, {'source_digest': None, 'base_digest': None})
                self.assertTrue(reasons)
            run.assert_not_called()
