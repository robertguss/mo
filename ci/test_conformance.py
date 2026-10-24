#!/usr/bin/env python3
"""Exercise the conformance check through its command-line interface."""

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

HEADER = "rule\taccepts\trefuses\tstops\tgap\tdrivers\tcost\tuntestable\n"


class ConformanceCheck(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        for name in ("ci", "docs/reference", "tests/conformance", "tests/golden/good",
                     "tests/errors/e0053"):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(pathlib.Path(__file__).with_name("check_conformance.py"),
                        self.root / "ci/check_conformance.py")
        self.reference = self.root / "docs/reference/README.md"
        self.reference.write_text("## 3. Values\n\nValues never change.\n")
        self.index = self.root / "tests/conformance/rules.tsv"
        self.index.write_text(HEADER + "3/1\tgood\te0053\t\t\n")
        self.snapshot = self.root / "tests/conformance/reference.tsv"
        self.snapshot.write_text("3/1\tValues never change.\n")
        (self.root / "tests/golden/good/main.mo").write_text("module Main\nfn main()\nend\n")
        (self.root / "tests/golden/good/stdout").write_text("")
        (self.root / "tests/errors/e0053/main.mo").write_text("module Main\nx = 1 + true\n")
        (self.root / "tests/errors/e0053/expected").write_text("error E0053: different types\n")

    def command(self, *args):
        return subprocess.run([sys.executable, str(self.root / "ci/check_conformance.py"), *args],
                              capture_output=True, text=True, timeout=10)

    def rejected(self, message):
        result = self.command()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(message, result.stderr)

    def test_valid_index_and_explicit_gap(self):
        result = self.command()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1 reference rules indexed; 0 have explicit gaps", result.stdout)
        self.index.write_text(HEADER + "3/1\t\t\t\tNeeds a program.\n")
        result = self.command()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1 have explicit gaps", result.stdout)

    def test_driver_cost_and_untestable_rows_are_not_gaps(self):
        (self.root / "ci/check_x.py").write_text(
            "def alignment(binary):\n    pass\n\nalignment(None)\n")
        (self.root / "tests/test_heap.c").write_text(
            "static void test_lists(struct fixture *f) {\n}\nint main(void) {\n"
            "    test_lists(NULL);\n}\n")
        (self.root / "Makefile").write_text("grammar: build/mo-grammar\n")
        self.index.write_text(
            HEADER +
            "3/1\t\t\t\t\tci/check_x.py:alignment,Makefile:grammar\t"
            "tests/test_heap.c:test_lists,tests/test_heap.c\tThe real clock varies.\n")
        result = self.command()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1 reference rules indexed; 0 have explicit gaps; 1 name other drivers; "
                      "1 have cost claims checked for semantics only; 1 untestable", result.stdout)
        result = self.command("--section", "3")
        self.assertIn("cost (semantics checked, cost not measured): tests/test_heap.c", result.stdout)

    def test_renamed_or_missing_driver_test_fails(self):
        driver = self.root / "ci/check_x.py"
        driver.write_text("def alignment(binary):\n    pass\n\nalignment(None)\n")
        self.index.write_text(HEADER +
                              "3/1\t\t\t\t\tci/check_x.py:alignment\n")
        self.assertEqual(self.command().returncode, 0)
        driver.write_text("def aligned(binary):\n    pass\n\naligned(None)\n")
        self.rejected("3/1: no test alignment in ci/check_x.py")
        driver.write_text("def alignment(binary):\n    pass\n")
        self.rejected("3/1: no test alignment in ci/check_x.py")
        driver.unlink()
        self.rejected("3/1: missing driver ci/check_x.py")
        self.index.write_text(HEADER +
                              "3/1\t\t\t\t\tci/check_x.py\n")
        self.rejected("3/1: driver evidence must name a test")
        self.index.write_text(HEADER +
                              "3/1\tgood\t\t\t\t\ttests/test_gone.c:lists\n")
        self.rejected("3/1: missing driver tests/test_gone.c")
        (self.root / "Makefile").write_text("grammar-check: x\n")
        self.index.write_text(HEADER +
                              "3/1\t\t\t\t\tMakefile:grammar\n")
        self.rejected("3/1: no test grammar in Makefile")

    def test_section_report_includes_subsections_and_exact_diagnostics(self):
        self.reference.write_text("## 3. Values\n\nValues never change.\n\n"
                                  "### 3.1 Numbers\n\nNumbers add.\n\n"
                                  "## 4. Bindings\n\nBindings name values.\n")
        self.snapshot.write_text("3/1\tValues never change.\n3.1/1\tNumbers add.\n"
                                 "4/1\tBindings name values.\n")
        with self.index.open("a") as stream:
            stream.write("3.1/1\tgood\t\t\t\n4/1\tgood\t\t\t\n")
        diagnostics = self.root / "tests/check/diagnostics"
        diagnostics.mkdir(parents=True)
        (diagnostics / "e0053.diag").write_text("E0053\n")
        result = self.command("--section", "3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("3/1 Values never change.", result.stdout)
        self.assertIn("3.1/1 Numbers add.", result.stdout)
        self.assertIn("tests/check/diagnostics/e0053.diag", result.stdout)
        self.assertNotIn("Bindings name values.", result.stdout)
        result = self.command("--section", "30")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown section: 30", result.stderr)

    def test_unmapped_rule_and_empty_row_fail(self):
        self.index.write_text(HEADER)
        self.rejected("reference rules with no entry: 3/1")
        self.index.write_text(HEADER + "3/1\t\t\t\t\n")
        self.rejected("no program or gap explanation")

    def test_reference_addition_or_change_requires_review(self):
        self.reference.write_text("## 3. Values\n\nValues sometimes change.\n")
        self.rejected("reference changed")
        self.reference.write_text("## 3. Values\n\nValues never change.\n\nAnother rule.\n")
        self.rejected("reference changed")

    def test_missing_program_output_and_exact_error_fail(self):
        program = self.root / "tests/golden/good/main.mo"
        program.unlink()
        self.rejected("missing source")
        program.write_text("module Main\n")
        output = self.root / "tests/golden/good/stdout"
        output.unlink()
        self.rejected("missing exact output")
        output.write_text("")
        (self.root / "tests/errors/e0053/expected").unlink()
        self.rejected("missing exact error")

    def test_wrong_verdict_and_duplicate_fail(self):
        status = self.root / "tests/golden/good/status"
        status.write_text("70\n")
        self.rejected("wrong status for accepts")
        status.unlink()
        with self.index.open("a") as stream:
            stream.write("3/1\tgood\t\t\t\n")
        self.rejected("duplicate rule")

    def test_inventory_includes_bullets_tables_and_text_not_example_comments(self):
        self.reference.write_text(
            "## 2. Source\n\n### 2.3 Names\n\n- First line\n  continues.\n- Second rule.\n\n"
            "```mo\n## Not a section\nmodule Demo\n```\n\n"
            "```text\nfn var\n```\n\n| Type | Values |\n| --- | --- |\n| `Int` | whole |\n\n"
            "## Appendix B. Grammar\n\nA final rule.\n")
        result = self.command("--inventory")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout,
                         "2.3/1\t- First line continues.\n2.3/2\t- Second rule.\n"
                         "2.3/3\tfn var\n2.3/4\t| `Int` | whole |\nB/1\tA final rule.\n")


if __name__ == "__main__":
    unittest.main()
