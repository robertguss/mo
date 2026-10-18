#!/usr/bin/env python3
"""Tests for learnability/wilson.py. Run: python3 learnability/test_wilson.py"""

import fractions
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import wilson  # noqa: E402


def run(*args):
    return subprocess.run([sys.executable, str(here / "wilson.py"), *args],
                          capture_output=True, text=True, check=False, timeout=30)


class Interval(unittest.TestCase):
    def test_newcombe_examples(self):
        # Newcombe (1998), "Two-sided confidence intervals for the single proportion",
        # Statistics in Medicine 17, table I, method 3 (score interval without correction).
        for k, n, low, high in [(81, 263, 0.2553, 0.3662), (15, 148, 0.0624, 0.1605),
                                (0, 20, 0.0, 0.1611), (1, 29, 0.0061, 0.1718)]:
            got = wilson.wilson(k, n)
            self.assertAlmostEqual(got[0], low, places=4, msg=f"{k}/{n}")
            self.assertAlmostEqual(got[1], high, places=4, msg=f"{k}/{n}")

    def test_none_and_all(self):
        # With no successes the upper bound is z^2 / (n + z^2), and the interval mirrors at all.
        z2 = wilson.Z ** 2
        self.assertEqual(wilson.wilson(0, 10), (0.0, wilson.wilson(0, 10)[1]))
        self.assertAlmostEqual(wilson.wilson(0, 10)[1], z2 / (10 + z2), places=12)
        self.assertEqual(wilson.wilson(10, 10)[1], 1.0)
        self.assertAlmostEqual(wilson.wilson(10, 10)[0], 10 / (10 + z2), places=12)

    def test_mirror(self):
        for n in (1, 6, 37, 140):
            for k in range(n + 1):
                low, high = wilson.wilson(k, n)
                other = wilson.wilson(n - k, n)
                self.assertAlmostEqual(low, 1 - other[1], places=12)
                self.assertLessEqual(low, k / n)
                self.assertGreaterEqual(high, k / n)

    def test_bad_counts(self):
        for k, n in [(0, 0), (1, 0), (-1, 5), (6, 5)]:
            with self.assertRaisesRegex(ValueError, "need 0 <= K <= N"):
                wilson.wilson(k, n)

    def test_quantile(self):
        # The two-sided 95% normal quantile, 1.959963984540054 to the printed digits.
        self.assertAlmostEqual(wilson.Z, 1.959963984540054, places=15)


class Verdict(unittest.TestCase):
    def test_three_verdicts(self):
        target = fractions.Fraction(85, 100)
        self.assertEqual(wilson.verdict(57, 60, target), "pass")  # [86.3%, 98.3%]
        self.assertEqual(wilson.verdict(51, 60, target), "point-estimate pass")  # exactly 85%
        self.assertEqual(wilson.verdict(50, 60, target), "miss")

    def test_tools_target_cannot_pass_on_six(self):
        # D229's "5 of 6" target: even 6 of 6 has a lower bound of 61%.
        target = fractions.Fraction(5, 6)
        self.assertEqual(wilson.verdict(6, 6, target), "point-estimate pass")
        self.assertEqual(wilson.verdict(5, 6, target), "point-estimate pass")
        self.assertEqual(wilson.verdict(4, 6, target), "miss")

    def test_protocol_boundaries(self):
        # The least passing cases protocol.md section 7 names, and one short of each.
        f = fractions.Fraction
        for k, n, target, want in [(22, 22, f(85, 100), "pass"),
                                   (21, 22, f(85, 100), "point-estimate pass"),
                                   (33, 34, f(85, 100), "pass"),
                                   (43, 45, f(85, 100), "pass"),
                                   (42, 45, f(85, 100), "point-estimate pass"),
                                   (180, 200, f(85, 100), "pass"),
                                   (162, 180, f(85, 100), "point-estimate pass"),
                                   (73, 73, f(95, 100), "pass"),
                                   (72, 72, f(95, 100), "point-estimate pass"),
                                   (109, 110, f(95, 100), "pass"),
                                   (108, 110, f(95, 100), "point-estimate pass")]:
            self.assertEqual(wilson.verdict(k, n, target), want, f"{k}/{n} against {target}")

    def test_miss_just_under(self):
        # 84 of 99 is 84.8%, under 85% by less than a point.
        self.assertEqual(wilson.verdict(84, 99, fractions.Fraction(85, 100)), "miss")
        self.assertEqual(wilson.verdict(85, 100, fractions.Fraction(85, 100)),
                         "point-estimate pass")


class Size(unittest.TestCase):
    def test_research_figure(self):
        # MO-20's research note: "about 140 per family" puts a 90% rate within about 5 points.
        self.assertEqual(wilson.size(0.9, 0.05), 141)
        self.assertLessEqual(wilson.centre_and_half(0.9, 141)[1], 0.05)
        self.assertGreater(wilson.centre_and_half(0.9, 140)[1], 0.05)

    def test_width_too_small(self):
        for width in (0.0009,):
            with self.assertRaises(ValueError):
                wilson.size(0.9, width)


class Kappa(unittest.TestCase):
    def test_textbook_example(self):
        # 50 items: both yes 20, A yes B no 5, A no B yes 10, both no 15.
        # Observed 0.7, expected 0.5, kappa 0.4.
        pairs = ([("yes", "yes")] * 20 + [("yes", "no")] * 5 + [("no", "yes")] * 10 +
                 [("no", "no")] * 15)
        value, observed = wilson.kappa(pairs)
        self.assertAlmostEqual(value, 0.4, places=12)
        self.assertAlmostEqual(observed, 0.7, places=12)

    def test_three_verdicts_and_perfect(self):
        pairs = [("pass", "pass"), ("close", "close"), ("fail", "fail"), ("pass", "pass")]
        self.assertAlmostEqual(wilson.kappa(pairs)[0], 1.0, places=12)

    def test_label_only_one_grader_uses(self):
        pairs = [("pass", "pass"), ("pass", "fail"), ("fail", "fail"), ("fail", "fail")]
        value, observed = wilson.kappa(pairs)
        self.assertAlmostEqual(observed, 0.75, places=12)
        # A: pass 1/2, fail 1/2; B: pass 1/4, fail 3/4; expected 1/8 + 3/8 = 1/2.
        self.assertAlmostEqual(value, 0.5, places=12)
        # A only ever says pass, so the fail B gives once is a label A never uses: kappa 0.
        value, _ = wilson.kappa([("pass", "pass"), ("pass", "fail"), ("pass", "pass")])
        self.assertAlmostEqual(value, 0.0, places=12)

    def test_one_verdict_only(self):
        value, observed = wilson.kappa([("pass", "pass")] * 5)
        self.assertTrue(math.isnan(value))
        self.assertEqual(observed, 1.0)


class Command(unittest.TestCase):
    def test_interval_with_target(self):
        result = run("5", "6", "--target", "5/6")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "5/6 = 83.3%, 95% Wilson interval [43.6%, 97.0%]; "
                                        "target 83.3%: point-estimate pass\n")

    def test_percent_target(self):
        result = run("57", "60", "--target", "85%")
        self.assertEqual(result.stdout, "57/60 = 95.0%, 95% Wilson interval [86.3%, 98.3%]; "
                                        "target 85.0%: pass\n")

    def test_size(self):
        self.assertEqual(run("--size", "90%", "5%").stdout, "141\n")

    def test_kappa_file(self):
        with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False) as f:
            f.write("Pass\tpass\nfail\tyes\n\nNO\t fail \nyes\tpass\n")
        result = run("--kappa", f.name)
        pathlib.Path(f.name).unlink()
        self.assertEqual(result.stdout, "kappa 0.500, observed agreement 75.0%, 4 programs\n")

    def test_kappa_file_errors(self):
        for text in ["yes\tpass\nmaybe\tfail\n", "pass\tpass\tpass\n", "pass\n"]:
            with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False) as f:
                f.write(text)
            result = run("--kappa", f.name)
            pathlib.Path(f.name).unlink()
            self.assertEqual(result.returncode, 2, text)
            self.assertIn(":", result.stderr, text)

    def test_usage_errors(self):
        for args in [(), ("5",), ("7", "6"), ("0", "0"), ("1_0", "20"), ("-1", "5"),
                     ("5", "6", "--goal", "1"), ("5", "6", "--target", "85"),
                     ("5", "6", "--target", "1/0"), ("5", "6", "--target", "much"),
                     ("--size", "2", "0.1"), ("--size", "0.9", "0"),
                     ("--kappa", "/nonexistent/file")]:
            result = run(*args)
            self.assertEqual(result.returncode, 2, args)
            self.assertTrue(result.stderr.startswith("wilson.py: "), args)
            self.assertEqual(result.stdout, "", args)


if __name__ == "__main__":
    unittest.main()
