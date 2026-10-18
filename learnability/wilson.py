#!/usr/bin/env python3
"""Interval and agreement arithmetic for the learnability protocol (learnability/protocol.md).

  python3 learnability/wilson.py K N [--target T]   K successes in N trials: the rate and its
                                                    95% Wilson interval, and, with a target,
                                                    the verdict the protocol gives it
  python3 learnability/wilson.py --size P W         the fewest trials whose 95% Wilson interval
                                                    around a true rate P is at most W either side
  python3 learnability/wilson.py --kappa FILE       Cohen's kappa between two graders: FILE has
                                                    one program per line, the two verdicts
                                                    separated by a tab

A rate or target is written as a fraction (0.85), a percentage (85%) or a ratio (5/6).
Verdicts against a target:
  pass                  the interval's lower bound is at or above the target
  point-estimate pass   the rate is at or above the target, but the lower bound is below it
  miss                  the rate is below the target
The exit status is 0 for any result and 2 for a usage error, so a script can print a miss.
"""

import fractions
import math
import sys

# The two-sided 95% normal quantile, the protocol's only confidence level.
Z = 1.959963984540054


def centre_and_half(p, n):
    """The centre and half-width of the Wilson interval for an observed rate p over n trials."""
    z2 = Z * Z
    centre = (p + z2 / (2 * n)) / (1 + z2 / n)
    half = Z / (1 + z2 / n) * math.sqrt(p * (1 - p) / n + z2 / (4 * n * n))
    return centre, half


def wilson(k, n):
    """The Wilson score interval for k successes in n trials, as (low, high)."""
    if n <= 0 or k < 0 or k > n:
        raise ValueError(f"need 0 <= K <= N and N > 0, got K={k} N={n}")
    centre, half = centre_and_half(k / n, n)
    low, high = centre - half, centre + half
    # At K=0 and K=N the bounds are exactly 0 and 1; rounding must not push them past.
    if k == 0:
        low = 0.0
    if k == n:
        high = 1.0
    return max(0.0, low), min(1.0, high)


def verdict(k, n, target):
    """The protocol's verdict for k of n against target, a Fraction."""
    low, _ = wilson(k, n)
    if fractions.Fraction(k, n) < target:
        return "miss"
    # Compare the bound with a little slack so a target equal to the bound isn't lost to rounding.
    if low >= float(target) - 1e-12:
        return "pass"
    return "point-estimate pass"


def size(rate, width):
    """The fewest trials whose Wilson interval around a true rate is at most width either side."""
    if not 0 <= rate <= 1 or width <= 0:
        raise ValueError("need 0 <= P <= 1 and W > 0")
    n = 1
    while centre_and_half(rate, n)[1] > width:
        n += 1
    return n


def kappa(pairs):
    """Cohen's kappa for a list of (verdict_a, verdict_b), as (kappa, observed agreement)."""
    if not pairs:
        raise ValueError("no programs to compare")
    n = len(pairs)
    observed = sum(1 for a, b in pairs if a == b) / n
    labels = sorted({label for pair in pairs for label in pair})
    expected = sum((sum(1 for a, _ in pairs if a == label) / n) *
                   (sum(1 for _, b in pairs if b == label) / n) for label in labels)
    if expected == 1:
        # Both graders gave every program the same one verdict: agreement is total but
        # kappa is undefined. The protocol treats it as a sample too narrow to judge.
        return float("nan"), observed
    return (observed - expected) / (1 - expected), observed


def parse_rate(text):
    """A fraction, percentage or ratio, as a Fraction."""
    try:
        if text.endswith("%"):
            value = fractions.Fraction(text[:-1]) / 100
        else:
            value = fractions.Fraction(text)
    except (ValueError, ZeroDivisionError):
        raise ValueError(f"not a rate: {text}") from None
    if not 0 <= value <= 1:
        raise ValueError(f"a rate is between 0 and 1: {text}")
    return value


def percent(x):
    return f"{100 * x:.1f}%"


def read_pairs(path):
    pairs = []
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip():
                continue
            fields = line.split("\t")
            if len(fields) != 2:
                raise ValueError(f"{path}:{number}: want two verdicts separated by a tab")
            pairs.append((fields[0].strip(), fields[1].strip()))
    return pairs


def main(args):
    if len(args) == 3 and args[0] == "--size":
        rate, width = parse_rate(args[1]), parse_rate(args[2])
        print(size(float(rate), float(width)))
        return 0
    if len(args) == 2 and args[0] == "--kappa":
        value, observed = kappa(read_pairs(args[1]))
        shown = "undefined" if math.isnan(value) else f"{value:.3f}"
        print(f"kappa {shown}, observed agreement {percent(observed)}")
        return 0
    if len(args) in (2, 4) and not args[0].startswith("-"):
        k, n = int(args[0]), int(args[1])
        low, high = wilson(k, n)
        line = f"{k}/{n} = {percent(k / n)}, 95% Wilson interval [{percent(low)}, {percent(high)}]"
        if len(args) == 4:
            if args[2] != "--target":
                raise ValueError(f"unknown option {args[2]}")
            target = parse_rate(args[3])
            line += f"; target {percent(float(target))}: {verdict(k, n, target)}"
        print(line)
        return 0
    raise ValueError("usage: wilson.py K N [--target T] | --size P W | --kappa FILE")


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (ValueError, OSError) as error:
        print(f"wilson.py: {error}", file=sys.stderr)
        sys.exit(2)
