#!/usr/bin/env python3
"""Tests for learnability/phase4/compare.py. Run: python3 learnability/phase4/test_compare.py"""

import math
import pathlib
import subprocess
import sys
import unittest

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import compare  # noqa: E402


def run(*args):
    return subprocess.run([sys.executable, str(here / "compare.py"), *args],
                          capture_output=True, text=True, check=False, timeout=60)


class Interval(unittest.TestCase):
    def test_newcombe_examples(self):
        # Newcombe (1998), "Interval estimation for the difference between independent
        # proportions", Statistics in Medicine 17, table II, method 10. Each case is
        # (first round k, n, second round k, n, low, high) for the second minus the first.
        for k1, n1, k2, n2, low, high in [(48, 80, 56, 70, 0.0524, 0.3339),
                                          (3, 10, 9, 10, 0.1705, 0.8090),
                                          (0, 29, 5, 56, -0.0381, 0.1926)]:
            _, got_low, got_high = compare.difference(k1, n1, k2, n2)
            self.assertAlmostEqual(got_low, low, places=4, msg=f"{k1}/{n1} {k2}/{n2}")
            self.assertAlmostEqual(got_high, high, places=4, msg=f"{k1}/{n1} {k2}/{n2}")

    def test_square_and_add_by_hand(self):
        # 2/7 against 6/7, from the two Wilson intervals written out here, not from compare.py.
        z = 1.959963984540054
        def score(k, n):
            p = k / n
            centre = (p + z * z / (2 * n)) / (1 + z * z / n)
            half = z / (1 + z * z / n) * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
            return centre - half, centre + half
        l1, u1 = score(2, 7)
        l2, u2 = score(6, 7)
        d = 6 / 7 - 2 / 7
        low = d - math.sqrt((6 / 7 - l2) ** 2 + (u1 - 2 / 7) ** 2)
        high = d + math.sqrt((u2 - 6 / 7) ** 2 + (2 / 7 - l1) ** 2)
        got = compare.difference(2, 7, 6, 7)
        self.assertAlmostEqual(got[1], low, places=12)
        self.assertAlmostEqual(got[2], high, places=12)

    def test_equal_rates_are_not_improved(self):
        for k, n in [(0, 10), (5, 10), (10, 10), (7, 31)]:
            self.assertFalse(compare.improved(k, n, k, n))

    def test_worse_is_not_improved(self):
        self.assertFalse(compare.improved(15, 20, 5, 20))

    def test_verdict_needs_lower_bound_above_zero(self):
        # 48/80 -> 56/70 has a lower bound of +5.2 points; 0/29 -> 5/56 has one of -3.8.
        self.assertTrue(compare.improved(48, 80, 56, 70))
        self.assertFalse(compare.improved(0, 29, 5, 56))


class Power(unittest.TestCase):
    def test_power_is_the_sum_over_outcomes(self):
        # Enumerate every outcome pair directly instead of using room().
        p1, p2, n = 0.3, 0.7, 12
        expected = sum(math.comb(n, a) * p1 ** a * (1 - p1) ** (n - a) *
                       math.comb(n, b) * p2 ** b * (1 - p2) ** (n - b)
                       for a in range(n + 1) for b in range(n + 1)
                       if compare.difference(a, n, b, n)[1] > 0)
        self.assertAlmostEqual(compare.power(p1, p2, n), expected, places=12)

    def test_no_true_difference_rarely_improves(self):
        # With equal true rates the one-sided error stays near 2.5%.
        for p in (0.1, 0.35, 0.5, 0.8):
            for n in (10, 20, 40):
                self.assertLess(compare.power(p, p, n), 0.035, msg=f"p={p} n={n}")

    def test_size_is_the_first_n_reaching_power(self):
        n = compare.size(0.35, 0.75, 0.8)
        self.assertIsNotNone(n)
        self.assertGreaterEqual(compare.power(0.35, 0.75, n), 0.8)
        self.assertLess(compare.power(0.35, 0.75, n - 1), 0.8)

    def test_unreachable_size(self):
        self.assertIsNone(compare.size(0.5, 0.52, 0.99))


class Room(unittest.TestCase):
    def test_room_is_the_least_improving_count(self):
        least = compare.room(10, 20, 20)
        self.assertTrue(compare.improved(10, 20, least, 20))
        self.assertFalse(compare.improved(10, 20, least - 1, 20))

    def test_ceiling_baseline_has_no_room(self):
        # 18/20 leaves no final count out of 20 that shows an improvement.
        self.assertIsNone(compare.room(18, 20, 20))
        self.assertFalse(compare.improved(18, 20, 20, 20))


class Command(unittest.TestCase):
    def test_compare_line(self):
        result = run("48", "80", "56", "70")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "48/80 -> 56/70: difference +20.0 points, "
                         "95% Newcombe interval [+5.2, +33.4]: improved")

    def test_room_and_size_lines(self):
        self.assertEqual(run("--room", "18", "20", "20").stdout.strip(), "none of 0..20")
        self.assertEqual(run("--room", "10", "20", "20").stdout.strip(),
                         f"{compare.room(10, 20, 20)}/20")
        self.assertEqual(run("--size", "35%", "3/4", "0.8").stdout.strip(),
                         str(compare.size(0.35, 0.75, 0.8)))

    def test_usage_errors_exit_2(self):
        for args in [(), ("1", "2"), ("3", "2", "1", "2"), ("x", "2", "1", "2"),
                     ("--power", "2", "0.5", "10"), ("--room", "1", "0", "5")]:
            result = run(*args)
            self.assertEqual(result.returncode, 2, args)
            self.assertTrue(result.stderr.startswith("compare.py: "), args)


if __name__ == "__main__":
    unittest.main()
