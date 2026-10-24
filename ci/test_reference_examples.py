#!/usr/bin/env python3
"""Exercise the reference-example driver through its CLI and a real mo binary."""

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

MO = str(pathlib.Path(sys.argv.pop(1)).resolve())


class ReferenceExamples(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        (self.root / "ci").mkdir()
        (self.root / "docs/reference").mkdir(parents=True)
        shutil.copyfile(pathlib.Path(__file__).with_name("check_reference_examples.py"),
                        self.root / "ci/check_reference_examples.py")

    def command(self, document):
        (self.root / "docs/reference/README.md").write_text(document, encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(self.root / "ci/check_reference_examples.py"), MO],
            capture_output=True, text=True, timeout=30)

    def test_contexts_and_module_paths(self):
        result = self.command(
            "```mo\nmodule Shop.HTTP2Server\nfn main()\nend\n```\n"
            "```mo\nfn double(n: Int) : Int\n  n * 2\nend\n```\n"
            "```mo\nvar total = 0\nfor n in 1..4\n  total += n\nend\n"
            "assert total == 6\n```\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("3 blocks, 3 checked, 1 run, 0 fragments skipped, 0 failures", result.stdout)

    def test_declarations_allow_whitespace_but_not_text_contents(self):
        for space in ("  ", "\t"):
            with self.subTest(space=space):
                result = self.command(
                    f"```mo\nmodule{space}Main\nfn{space}main()\n"
                    '  stop "must run"\nend\n```\n')
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("mo run exited 70", result.stdout)
        result = self.command(
            '```mo\nusage = """\nmodule Other\nfn main()\nend\n"""\n```\n'
            '```mo\n# """ does not start text in a comment\nmodule Main\n'
            'fn main()\nend\n```\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("2 blocks, 2 checked, 1 run", result.stdout)

    def test_broken_block_and_failures_stay_in_document_order(self):
        result = self.command(
            "# Examples\n\n```mo\nfn wrong() : Int\n  1 + true\nend\n```\n\n"
            "```mo\nmodule Main\nfn main()\n  stop \"broken run\"\nend\n```\n")
        self.assertEqual(result.returncode, 1)
        failures = [line for line in result.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(len(failures), 2, result.stdout)
        self.assertIn("line 3:", failures[0])
        self.assertIn("E0053", failures[0])
        self.assertIn("line 9:", failures[1])
        self.assertIn("mo run exited 70", failures[1])
        self.assertTrue(result.stdout.rstrip().endswith("2 failures"), result.stdout)

    def test_output_is_compared_exactly(self):
        program = ('```mo\nfn main(console: Console)\n'
                   '  console.print("héllo ")\n  console.print("second")\nend\n```\n\n')
        result = self.command(program + "```\nhéllo \nsecond\n```\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for output in ("héllo\nsecond\n", "héllo \nwrong\n", "héllo \nsecond\n\n", ""):
            with self.subTest(output=output):
                result = self.command(program + "```\n" + output + "```\n")
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("FAIL line 1: output mismatch", result.stdout)
        # An empty output fence means zero bytes, not one newline.
        result = self.command("```mo\nfn main()\nend\n```\n```\n```\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_expected_error_requires_code_and_rejects_successful_forms(self):
        broken = "```mo\nfn wrong() : Int\n  1 + true\nend\n```\n"
        result = self.command("<!-- mo-example: error E0053 -->\n" + broken)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.command("<!-- mo-example: error E0050 -->\n" + broken)
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected E0050", result.stdout)
        self.assertIn("E0053", result.stdout)
        # Missing module and module-level statements fail, but the wrapper passes.
        result = self.command("<!-- mo-example: error E0031 -->\n"
                              "```mo\nassert true\n```\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected E0031, but checked", result.stdout)

    def test_fragment_marker_and_fence_boundaries(self):
        result = self.command(
            "````text\n```mo\nnot an example\n```\n````\n"
            "<!-- mo-example: fragment -->\n```mo\nmissing.context\n```\n"
            "```mo\nmodule Main\nfn main()\nend\n```\n"
            "This prose separates the next fence; it is not expected output.\n"
            "```\nnot output\n```\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("2 blocks, 1 checked, 1 run, 1 fragments skipped, 0 failures", result.stdout)
        result = self.command("<!-- mo-example: fragment -->\n\n```mo\nmissing.context\n```\n")
        self.assertEqual(result.returncode, 1, result.stdout)
        result = self.command("<!-- mo-example: fragement -->\n```mo\nmodule Main\n```\n")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("invalid mo-example marker", result.stdout)

    def test_missing_fence_or_examples_fails(self):
        for document in ("```mo\nmodule Main\n", "No examples.\n"):
            with self.subTest(document=document):
                result = self.command(document)
                self.assertEqual(result.returncode, 1, result.stdout)
        result = self.command("```mo\nfn main()\nend\n```\n```\n")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("unterminated output fence", result.stdout)
        result = self.command("```mo\nfn f()\nend\n```\n```\n```\n")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("output fence requires a complete program with main", result.stdout)


if __name__ == "__main__":
    unittest.main()
